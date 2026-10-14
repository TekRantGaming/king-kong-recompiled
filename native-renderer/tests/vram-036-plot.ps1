$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
$root='F:\KK-native-renderer\runs\task036-analysis'
$curves=Import-Csv "$root\curves.csv"
$summary=Import-Csv "$root\summary.csv" | Where-Object { [int]$_.samples -gt 0 -and $_.tour_kind -ne 'legacy-menu' }
$groups=$summary | Group-Object budget_gib,case,simulated_pressure,gtao,pressure_delay_s,tour_kind,backend
foreach ($group in $groups) {
  $chart=New-Object System.Windows.Forms.DataVisualization.Charting.Chart
  try {
    $chart.Width=1200; $chart.Height=700
    $area=New-Object System.Windows.Forms.DataVisualization.Charting.ChartArea
    $area.AxisX.Title='Seconds since first budget sample'; $area.AxisX.Minimum=0
    $area.AxisY.Title='Live local memory usage (MiB)'
    $area.AxisY.Minimum=0
    $chart.ChartAreas.Add($area)
    $chart.Legends.Add((New-Object System.Windows.Forms.DataVisualization.Charting.Legend))
    $first=$group.Group[0]
    $null=$chart.Titles.Add("Task 036: $($first.backend), $($first.budget_gib) GiB cap, $($first.case), synthetic pressure=$($first.simulated_pressure), GTAO=$($first.gtao)")
    foreach ($row in $group.Group) {
      $series=New-Object System.Windows.Forms.DataVisualization.Charting.Series $row.run
      $series.LegendText=$row.control
      $series.ChartType='Line'; $series.BorderWidth=2
      $series.Color=if ($row.control -eq 'before') {'SteelBlue'} else {'DarkOrange'}
      foreach ($sample in ($curves | Where-Object run -EQ $row.run)) {
        $null=$series.Points.AddXY([double]$sample.seconds,[double]$sample.usage/1MB)
      }
      $chart.Series.Add($series)
    }
    $name="$($first.backend)-$($first.budget_gib)-$($first.case)-$($first.simulated_pressure)-$($first.gtao)-delay$($first.pressure_delay_s)"
    $chart.SaveImage("$root\curve-$name.png",'Png')
  } finally { $chart.Dispose() }
}
