#include "coop_direct_udp.h"
#include "coop_protocol.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <deque>
#include <map>
#include <random>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kk::coop {
bool ValidJoinCode(std::string_view code) {
  if (code.size() != 6) return false;
  for (unsigned char c : code) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  return true;
}
std::string NormalizeJoinCode(std::string_view code) {
  std::string out(code);
  for (char& c : out) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  return out;
}
std::string NewJoinCode() {
  constexpr char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  std::random_device random;
  std::uniform_int_distribution<size_t> pick(0, sizeof(alphabet)-2);
  std::string out;
  for (int i=0; i<6; ++i) out += alphabet[pick(random)];
  return out;
}
bool UdpRateLimit::Take(double now, double count) {
  if (!std::isfinite(now) || !std::isfinite(count) || count < 0) return false;
  if (last_ >= 0 && now > last_) tokens_ = std::min(burst_, tokens_+(now-last_)*rate_);
  last_ = std::max(last_, now);
  if (count > tokens_) return false;
  tokens_ -= count; return true;
}
namespace {
#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket kInvalid = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
struct Network {
  bool ok = false;
  Network() { WSADATA data{}; ok = WSAStartup(MAKEWORD(2,2), &data)==0; }
  ~Network() { if (ok) WSACleanup(); }
};
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket kInvalid = -1;
void CloseSocket(Socket s) { close(s); }
struct Network { bool ok = true; };
#endif
constexpr uint32_t kWireMagic = 0x4B4B5532; // KKU2: session-nonce-scoped reliability envelope.
constexpr size_t kWireHeader = 28, kWindow = 64;
uint64_t Get(const uint8_t* p, size_t n) { uint64_t v=0; while (n--) v=(v<<8)|*p++; return v; }
void Put(uint8_t* p, uint64_t v, size_t n) { while (n) { p[--n]=uint8_t(v); v>>=8; } }
bool SameAddress(const sockaddr_storage& a, const sockaddr_storage& b) {
  if (a.ss_family != b.ss_family) return false;
  if (a.ss_family == AF_INET) {
    const auto& x=reinterpret_cast<const sockaddr_in&>(a); const auto& y=reinterpret_cast<const sockaddr_in&>(b);
    return x.sin_port==y.sin_port && x.sin_addr.s_addr==y.sin_addr.s_addr;
  }
  const auto& x=reinterpret_cast<const sockaddr_in6&>(a); const auto& y=reinterpret_cast<const sockaddr_in6&>(b);
  return x.sin6_port==y.sin6_port && x.sin6_scope_id==y.sin6_scope_id &&
      std::equal(std::begin(x.sin6_addr.s6_addr),std::end(x.sin6_addr.s6_addr),std::begin(y.sin6_addr.s6_addr));
}
class Udp final : public DirectUdpTransport {
 public:
  explicit Udp(DirectUdpOptions options) : options_(std::move(options)) {
    if (!network_.ok || !options_.port || !ValidJoinCode(options_.join_code)) return;
    std::random_device random;
    nonce_=(uint64_t(random())<<32)^random(); if (!nonce_) nonce_=1;
    addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM;
    hints.ai_flags=AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* result=nullptr;
    const auto port=std::to_string(options_.port);
    if (getaddrinfo(options_.address.c_str(), port.c_str(), &hints, &result)!=0) return;
    const int family=result->ai_family;
    sockaddr_storage local{};
    const auto length=AddressLength(result->ai_addrlen);
    std::copy_n(reinterpret_cast<const uint8_t*>(result->ai_addr),result->ai_addrlen,reinterpret_cast<uint8_t*>(&local));
    freeaddrinfo(result);
    socket_=socket(family, SOCK_DGRAM, IPPROTO_UDP); if (socket_==kInvalid) return;
    if (family==AF_INET6) {
      const int only=options_.address=="::" ? 0 : 1;
      if (setsockopt(socket_,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<const char*>(&only),sizeof(only))!=0) return;
    }
#ifdef _WIN32
    const int exclusive=1;
    if (setsockopt(socket_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))!=0) return;
#endif
    bool routed_join=false;
    if (!options_.host) {
      peer_=local; peer_length_=length; has_address_=true;
      // Joining loopback stays explicitly loopback. Public joins let UDP connect select
      // one local route/interface instead of opening a wildcard listener.
      if (family==AF_INET) {
        auto& a=reinterpret_cast<sockaddr_in&>(local); a.sin_port=0;
        routed_join=(ntohl(a.sin_addr.s_addr)>>24)!=127;
      } else {
        auto& a=reinterpret_cast<sockaddr_in6&>(local); a.sin6_port=0;
        routed_join=!IN6_IS_ADDR_LOOPBACK(&a.sin6_addr);
      }
    }
    if (routed_join) {
      if (connect(socket_,reinterpret_cast<const sockaddr*>(&peer_),peer_length_)!=0) return;
    } else if (bind(socket_,reinterpret_cast<sockaddr*>(&local),length)!=0) return;
    AddressLength bound_length=sizeof(local);
    if (getsockname(socket_,reinterpret_cast<sockaddr*>(&local),&bound_length)!=0) return;
    local_port_=family==AF_INET ? ntohs(reinterpret_cast<sockaddr_in&>(local).sin_port) :
        ntohs(reinterpret_cast<sockaddr_in6&>(local).sin6_port);
#ifdef _WIN32
    u_long mode=1; if (ioctlsocket(socket_,FIONBIO,&mode)!=0) return;
#else
    const int flags=fcntl(socket_,F_GETFL,0);
    if (flags<0 || fcntl(socket_,F_SETFL,flags|O_NONBLOCK)<0) return;
#endif
    state_=TransportState::Ready;
  }
  ~Udp() override { Close(); }
  bool Send(std::span<const uint8_t> data, bool reliable) override { return Queue(data,reliable,false); }
  bool SetChoice(LobbyChoice choice) override {
    if (!options_.host || !choice.valid() || choice.revision <= choice_.revision) return false;
    std::array<uint8_t,11> data{}; data[0]=2; Put(data.data()+1,choice.revision,8);
    data[9]=uint8_t(choice.chapter); data[10]=choice.continue_game;
    if (!Queue(data,true,true)) return false;
    choice_=choice; local_ready_=peer_ready_=false; return true;
  }
  LobbyChoice Choice() const override { return choice_; }
  bool SetReady(bool ready, uint64_t revision) override {
    if (revision && revision != choice_.revision) return false;
    if (ready==local_ready_) return true;
    std::array<uint8_t,9> value{}; value[0]=ready; Put(value.data()+1,choice_.revision,8);
    if (!Queue(value,true,true)) return false;
    local_ready_=ready; return true;
  }
  bool PeerReady() const override { return peer_ready_; }
  bool CodeRejected() const override { return code_rejected_; }
  uint16_t LocalPort() const override { return local_port_; }
  bool Receive(std::vector<uint8_t>& out) override {
    if (ready_.empty()) return false;
    out=std::move(ready_.front()); ready_.pop_front(); return true;
  }
  void Poll(double now) override {
    if (state_!=TransportState::Ready || !std::isfinite(now)) return;
    now_=now; DrainOrdered();
    for (auto it=delayed_.begin(); it!=delayed_.end();) {
      if (it->due>now) { ++it; continue; }
      SendDatagram(it->wire,it->address,it->length); it=delayed_.erase(it);
    }
    for (int i=0; i<64; ++i) {
      std::array<uint8_t,kMaxPacketBytes+kWireHeader+1> wire{};
      sockaddr_storage from{}; AddressLength length=sizeof(from);
      const int n=int(recvfrom(socket_,reinterpret_cast<char*>(wire.data()),int(wire.size()),0,
          reinterpret_cast<sockaddr*>(&from),&length));
      if (n<0) {
#ifdef _WIN32
        const auto error=WSAGetLastError();
        if (error==WSAEMSGSIZE) { ++stats_.malformed; continue; }
        if (error!=WSAEWOULDBLOCK && error!=WSAECONNRESET) state_=TransportState::Failed;
#else
        if (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=ECONNREFUSED) state_=TransportState::Failed;
#endif
        break;
      }
      if (has_address_ && !SameAddress(from,peer_)) continue; // One peer, forever, until explicit Leave.
      if (!(peer_nonce_ ? incoming_.Take(now) : attempts_.Take(now))) continue;
      if (n<int(kWireHeader) || n>int(kMaxPacketBytes+kWireHeader) || Get(wire.data(),4)!=kWireMagic ||
          Get(wire.data()+8,2)!=size_t(n)-kWireHeader || wire[10]>3 || wire[11]>1) {
        ++stats_.malformed; continue;
      }
      const uint32_t seq=uint32_t(Get(wire.data()+4,4));
      const uint8_t kind=wire[10]; const bool control=wire[11]!=0;
      const uint64_t source=Get(wire.data()+12,8), target=Get(wire.data()+20,8);
      const std::span<const uint8_t> payload(wire.data()+kWireHeader,size_t(n)-kWireHeader);
      if (!source || (target!=nonce_ && !(options_.host && !peer_nonce_ && !target)) ||
          (peer_nonce_ && source!=peer_nonce_)) continue;
      if (kind==3) {
        if (!options_.host && !peer_nonce_ && target==nonce_ && payload.size()==1 && payload[0]==1) {
          code_rejected_=true; state_=TransportState::Failed;
        }
        continue;
      }
      if ((kind==2 && (control || !payload.empty() || !seq)) || (kind!=2 && payload.empty()) ||
          (kind==0 && (seq || control)) || (kind==1 && !seq) ||
          (control && !((payload.size()==9 && payload[0]<=1) ||
            (payload.size()==11 && payload[0]==2 && payload[9]<42 && payload[10]<=1 && Get(payload.data()+1,8))))) { ++stats_.malformed; continue; }
      if (!peer_nonce_) {
        Message hello;
        if (kind!=1 || control || seq!=1 || !Decode(payload,hello) || hello.type!=MessageType::Hello ||
            (options_.host ? hello.epoch!=0 : hello.epoch==0)) { ++stats_.malformed; continue; }
        if (NormalizeJoinCode(hello.join_code)!=NormalizeJoinCode(options_.join_code)) {
          // Bounded, smaller-than-request response. Do not pin or consume reliable sequence 1.
          if (options_.host && rejects_.Take(now)) {
            auto reject=Wire(3,0,false,std::array<uint8_t,1>{1});
            Put(reject.data()+20,source,8); TransmitTo(reject,from,length);
          } else if (!options_.host) { code_rejected_=true; state_=TransportState::Failed; }
          continue;
        }
        peer_=from; peer_length_=length; has_address_=true; peer_nonce_=source;
      }
      ++stats_.received;
      if (kind==2) {
        auto p=pending_.find(seq);
        if (p!=pending_.end()) {
          if (p->second.last>=0 && p->second.attempts==1) {
            const double rtt=(now-p->second.last)*1000;
            stats_.ping_ms=stats_.ping_ms>0 ? .8*stats_.ping_ms+.2*rtt : rtt;
          }
          pending_.erase(p);
        }
      } else if (kind==0) {
        if (ready_.size()<kWindow) ready_.emplace_back(payload.begin(),payload.end());
      } else {
        const int32_t delta=int32_t(seq-next_receive_);
        if (delta>=int32_t(kWindow)) continue;
        if (delta>=0) ordered_.try_emplace(seq,Incoming{control,{payload.begin(),payload.end()}});
        auto ack=Wire(2,seq,false,{}); Transmit(ack); DrainOrdered();
      }
    }
    if (!has_address_) return;
    for (auto& [seq,p] : pending_) {
      if (p.last>=0 && now-p.last<.15) continue;
      if (Transmit(p.wire)) { if (p.attempts++) ++stats_.resent; p.last=now; }
    }
    stats_.loss=stats_.sent ? double(stats_.resent)/double(stats_.sent) : 0;
  }
  TransportState State() const override { return state_; }
  TransportStats Stats() const override { return stats_; }
  void Close() override {
    if (socket_!=kInvalid) { CloseSocket(socket_); socket_=kInvalid; }
    state_=TransportState::Closed; peer_ready_=false;
    ready_.clear(); ordered_.clear(); pending_.clear();
    delayed_.clear();
  }
 private:
  struct Pending { std::vector<uint8_t> wire; double last=-1; uint32_t attempts=0; };
  struct Incoming { bool control; std::vector<uint8_t> data; };
  std::vector<uint8_t> Wire(uint8_t kind,uint32_t seq,bool control,std::span<const uint8_t> data) {
    std::vector<uint8_t> wire(kWireHeader+data.size());
    Put(wire.data(),kWireMagic,4); Put(wire.data()+4,seq,4); Put(wire.data()+8,data.size(),2);
    wire[10]=kind; wire[11]=control;
    Put(wire.data()+12,nonce_,8); Put(wire.data()+20,peer_nonce_,8);
    std::copy(data.begin(),data.end(),wire.begin()+kWireHeader); return wire;
  }
  bool Queue(std::span<const uint8_t> data,bool reliable,bool control) {
    if (state_!=TransportState::Ready || data.empty() || data.size()>kMaxPacketBytes) return false;
    if (reliable && pending_.size()>=kWindow) { state_=TransportState::Failed; return false; }
    const uint32_t seq=reliable ? next_send_++ : 0;
    if (reliable && !seq) { state_=TransportState::Failed; return false; }
    auto wire=Wire(reliable ? 1 : 0,seq,control,data);
    if (reliable) pending_.emplace(seq,Pending{std::move(wire)});
    else if (has_address_ && peer_nonce_) Transmit(wire);
    return true;
  }
  void DrainOrdered() {
    while (ready_.size()<kWindow) {
      auto it=ordered_.find(next_receive_); if (it==ordered_.end()) break;
      if (it->second.control) {
        const auto& data=it->second.data; const uint64_t revision=Get(data.data()+1,8);
        if (data[0]==2) {
          if (!options_.host && revision>choice_.revision) {
            choice_={revision,int(data[9]),bool(data[10])}; local_ready_=peer_ready_=false;
          }
        } else if (revision==choice_.revision) peer_ready_=data[0]!=0;
      }
      else ready_.push_back(std::move(it->second.data));
      ordered_.erase(it); ++next_receive_;
    }
  }
  bool Transmit(std::vector<uint8_t>& wire) {
    Put(wire.data()+20,peer_nonce_,8);
    return TransmitTo(wire,peer_,peer_length_);
  }
  bool TransmitTo(std::span<const uint8_t> wire,const sockaddr_storage& address,AddressLength length) {
    if (!outgoing_.Take(now_)) return false;
    const auto& sim=options_.simulation;
    if (sim.delay_ms>0 || sim.jitter_ms>0 || sim.loss_percent>0) {
      auto random=[&] { simulation_rng_=1664525u*simulation_rng_+1013904223u; return double(simulation_rng_)/4294967296.; };
      if (random()*100<sim.loss_percent) { ++stats_.sent; return true; }
      const double delay=std::max(0.,sim.delay_ms+(2*random()-1)*sim.jitter_ms)/1000.;
      if (delayed_.size()>=512) return false;
      delayed_.push_back({now_+delay,{wire.begin(),wire.end()},address,length});
      return true;
    }
    return SendDatagram(wire,address,length);
  }
  bool SendDatagram(std::span<const uint8_t> wire,const sockaddr_storage& address,AddressLength length) {
    const int n=int(sendto(socket_,reinterpret_cast<const char*>(wire.data()),int(wire.size()),0,
        reinterpret_cast<const sockaddr*>(&address),length));
    if (n==int(wire.size())) { ++stats_.sent; return true; } return false;
  }
  Network network_; DirectUdpOptions options_;
  Socket socket_=kInvalid; sockaddr_storage peer_{}; AddressLength peer_length_=0;
  uint16_t local_port_=0;
  bool has_address_=false, peer_ready_=false, local_ready_=false, code_rejected_=false;
  uint64_t nonce_=0,peer_nonce_=0;
  LobbyChoice choice_;
  uint32_t next_send_=1,next_receive_=1;
  double now_=0;
  TransportState state_=TransportState::Failed; TransportStats stats_;
  UdpRateLimit incoming_{512,1024}, outgoing_{512,1024}, attempts_{20,40}, rejects_{2,4};
  std::map<uint32_t,Pending> pending_;
  std::map<uint32_t,Incoming> ordered_;
  std::deque<std::vector<uint8_t>> ready_;
  struct Delayed { double due; std::vector<uint8_t> wire; sockaddr_storage address; AddressLength length; };
  std::deque<Delayed> delayed_;
  uint32_t simulation_rng_=options_.simulation.seed;
};
} // namespace
std::unique_ptr<DirectUdpTransport> CreateDirectUdp(DirectUdpOptions options) { return std::make_unique<Udp>(std::move(options)); }
std::string DirectLocalAddresses() {
  Network network; if (!network.ok) return {};
  std::array<char,256> name{}; if (gethostname(name.data(),int(name.size()))!=0) return {};
  addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM;
  addrinfo* result=nullptr; if (getaddrinfo(name.data(),nullptr,&hints,&result)!=0) return {};
  std::string out;
  for (auto* p=result; p && out.size()<180; p=p->ai_next) {
    std::array<char,NI_MAXHOST> address{};
    if (getnameinfo(p->ai_addr,AddressLength(p->ai_addrlen),address.data(),int(address.size()),nullptr,0,NI_NUMERICHOST)!=0) continue;
    if (!out.empty()) out += ", "; out += address.data();
  }
  freeaddrinfo(result); return out;
}
} // namespace kk::coop
