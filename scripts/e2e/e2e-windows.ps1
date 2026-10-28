$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo '.rivet\acceptance\windows'
$appData = Join-Path $output 'appdata'
$data = Join-Path $appData 'Payback'
New-Item -ItemType Directory -Force -Path $output,$data | Out-Null
$fixture = '{"version":1,"devices":[{"id":"acceptance-device","name":"Codex Acceptance Device","icon":"box","category":"computer","priceMinor":1299900,"currency":"CNY","purchaseDate":"2024-06-01","willingPerDayMinor":3000,"notes":"","createdAt":"2026-01-01 00:00:00","updatedAt":"2026-01-01 00:00:00"}],"settings":{"currency":"CNY","updateAutoCheck":false,"updateBaseUrl":null,"lastUpdateCheckAt":null,"rolloutBucket":null,"seenMilestones":{"acceptance-device":["days-100","days-365","paid-back"]}}}'
# BOM-free UTF-8: a BOM makes read-json throw, the v1 migration silently
# skips the file, and the seeded device never appears.
[IO.File]::WriteAllText((Join-Path $data 'payback.json'), $fixture)
$exe = Get-ChildItem -LiteralPath (Join-Path $repo 'dist') -Filter RivetHost.exe -File -Recurse | Select-Object -First 1
if (-not $exe) { throw 'packaged Payback executable was not found' }
function Wait-Until([string]$Description,[scriptblock]$Probe,[int]$TimeoutSeconds=90) { $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds);while([DateTime]::UtcNow -lt $deadline){$v=& $Probe;if($null -ne $v -and $v -ne $false){return $v};Start-Sleep -Milliseconds 100};throw "timed out waiting for $Description" }
function Contains-Text($Root,[string]$Fragment) { $nodes=$Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt $nodes.Count;$i++){if($nodes[$i].Current.Name -like "*$Fragment*"){return $true}};return $false }
function Save-Snapshot($Root,[string]$Path) { $nodes=$Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition);$snapshot=for($i=0;$i -lt [Math]::Min($nodes.Count,800);$i++){$n=$nodes[$i];[ordered]@{name=$n.Current.Name;controlType=$n.Current.ControlType.ProgrammaticName;automationId=$n.Current.AutomationId}};ConvertTo-Json -InputObject @($snapshot) -Depth 4 | Set-Content -LiteralPath $Path -Encoding utf8 }
$process=$null;$oldAppData=$env:APPDATA
try { $env:APPDATA=$appData;$process=Start-Process -FilePath $exe.FullName -PassThru -WorkingDirectory $repo
  $desktop=[System.Windows.Automation.AutomationElement]::RootElement;$window=Wait-Until 'Payback window' {$c=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$process.Id);$desktop.FindFirst([System.Windows.Automation.TreeScope]::Children,$c)}
  try{[void](Wait-Until 'seeded backend device' {Contains-Text $window 'Codex Acceptance Device'})}catch{Save-Snapshot $window (Join-Path $output 'accessibility-failure.json');throw}
  Save-Snapshot $window (Join-Path $output 'accessibility.json');[ordered]@{platform='windows';actions=@('launch:seeded-device');assertions=@('packaged WinUI window activated','seeded device crossed RVT1 into native accessibility tree')}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $output 'interaction-trace.json') -Encoding utf8
  $pattern=$window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern);([System.Windows.Automation.WindowPattern]$pattern).Close();[void](Wait-Until 'process exit' {$process.Refresh();$process.HasExited} 30)
} finally { $env:APPDATA=$oldAppData;if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue};if($process){$process.Dispose()} }
