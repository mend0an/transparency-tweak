# Reads current DLL pages and compares live glass / plain / glass performance.
# Restores AeroGlass exactly; leaves the user's transparency percentage unchanged.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot '..\out\aero-prototype\current-performance.json'
$key = 'HKCU:\Software\TransparencyTweak'
$saved = Get-ItemProperty -LiteralPath $key
$hadAero = $null -ne $saved.PSObject.Properties['AeroGlass']
$session = (Get-Process -Id $PID).SessionId
$cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.ComponentModel;
public static class AeroMemorySensor {
 [StructLayout(LayoutKind.Sequential)] public struct Page { public IntPtr Address; public UIntPtr Flags; }
 [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 [DllImport("psapi.dll", SetLastError=true)] static extern bool QueryWorkingSetEx(IntPtr process,[In,Out] Page[] pages,uint bytes);
 public static long[] Pages(int pid,long start,int size) {
  int pageSize=Environment.SystemPageSize;
  Page[] pages=new Page[(size+pageSize-1)/pageSize];
  for(int i=0;i<pages.Length;i++) pages[i].Address=new IntPtr(start+(long)i*pageSize);
  IntPtr h=OpenProcess(0x400,false,pid);
  if(h==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
  try {
   if(!QueryWorkingSetEx(h,pages,(uint)(pages.Length*Marshal.SizeOf(typeof(Page))))) throw new Win32Exception(Marshal.GetLastWin32Error());
   long resident=0,privateResident=0;
   foreach(Page p in pages){ulong flags=p.Flags.ToUInt64();if((flags&1)!=0){resident+=pageSize;if((flags&0x8000)==0)privateResident+=pageSize;}}
   return new long[]{resident,privateResident};
  } finally {CloseHandle(h);}
 }
}
'@
$moduleRows = foreach ($process in @(Get-Process explorer,SystemSettings -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session)) {
    foreach ($module in @($process.Modules | Where-Object ModuleName -like 'TweakTap*')) {
        $pages = [AeroMemorySensor]::Pages($process.Id,$module.BaseAddress.ToInt64(),$module.ModuleMemorySize)
        [pscustomobject]@{PID=$process.Id;Process=$process.ProcessName;DLL=$module.FileName;SHA256=(Get-FileHash -LiteralPath $module.FileName).Hash;MappedBytes=$module.ModuleMemorySize;ResidentImageBytes=$pages[0];PrivateResidentImageBytes=$pages[1]}
    }
}
$phases = @()
try {
    foreach ($phase in @(@{Name='glass_before';Value=1},@{Name='plain';Value=0},@{Name='glass_after';Value=1})) {
        Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value $phase.Value -Type DWord
        Start-Sleep -Seconds 2
        $targets = @(Get-Process explorer,dwm,SystemSettings,Taskmgr -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session)
        $targetIds = @($targets.Id)
        # Processes may exit while the wildcard counter set is sampled. Keep
        # valid samples, explicitly drop invalid rows instead of aborting all.
        $sets = @(Get-Counter -Counter '\Process(*)\ID Process','\Process(*)\% Processor Time','\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage' -SampleInterval 1 -MaxSamples 2 -ErrorAction SilentlyContinue)
        if (-not $sets.Count) { throw 'Performance counters returned no samples.' }
        $last = @($sets[-1].CounterSamples | Where-Object Status -eq 0)
        $dropped = @($sets[-1].CounterSamples | Where-Object Status -ne 0).Count
        $ids = @{}
        foreach ($counter in $last) {
            if ($counter.Path.ToLowerInvariant().EndsWith('\id process')) { $ids[$counter.InstanceName] = [int]$counter.CookedValue }
        }
        $cpu = @{}
        $gpu = @()
        foreach ($counter in $last) {
            if ($counter.Path.ToLowerInvariant().EndsWith('\% processor time') -and $ids.ContainsKey($counter.InstanceName)) {
                $target = $ids[$counter.InstanceName]
                if ($targetIds -contains $target) { $cpu[$target] = $counter.CookedValue / $cores }
            } elseif ($counter.InstanceName -match '^pid_(\d+)_') {
                $target = [int]$Matches[1]
                if ($targetIds -contains $target) { $gpu += [pscustomobject]@{PID=$target;Path=$counter.Path;Bytes=[int64]$counter.CookedValue} }
            }
        }
        $hosts = foreach ($process in $targets) {
            $process.Refresh()
            [pscustomobject]@{PID=$process.Id;Name=$process.ProcessName;PrivateCommitBytes=$process.PrivateMemorySize64;WorkingSetBytes=$process.WorkingSet64;CPUPercent=$cpu[$process.Id];Threads=$process.Threads.Count}
        }
        $phases += [pscustomobject]@{Name=$phase.Name;TimestampUTC=[DateTime]::UtcNow.ToString('o');Hosts=@($hosts);GPU=$gpu;InvalidCounterRowsDropped=$dropped}
        Write-Host ('Captured ' + $phase.Name)
    }
} finally {
    if ($hadAero) { Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value $saved.AeroGlass -Type DWord }
    else { Remove-ItemProperty -LiteralPath $key -Name AeroGlass -ErrorAction SilentlyContinue }
}
[pscustomobject]@{
    TimestampUTC=[DateTime]::UtcNow.ToString('o')
    TransparencyPercent=$saved.TaskbarTransparency
    LogicalProcessors=$cores
    ModuleImages=@($moduleRows)
    Phases=$phases
    Limitations=@('DLL image pages exclude heap, stacks and compositor resources.','Host counters include unrelated activity; GPU counters can include cross-process shared allocations.','Short warm A/B/A samples do not measure Task Manager startup latency or prove causation.')
} | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host "Saved $out; original AeroGlass value restored."
