# VC Mod Manager - installs, updates, disables, enables and removes optional
# enhancement mods for GTA Vice City 1.0 next to VC Modern Fix, from each mod's
# official GitHub releases.
#
# Run VCModManager.bat in the game folder (next to gta-vc.exe). On the first run it
# asks whether you want the extra mods and which ones. Afterwards it shows a menu.
#
# Rules it follows:
# - Only each mod's own files are installed, from an explicit list per mod. Files a
#   mod's archive carries for other setups (its own ASI loader as d3d8.dll, a
#   scripts\global.ini that would stop the root folder's .asi files loading) are
#   left out: VC Modern Fix is d3d8.dll, and the game folder already has its loader.
# - A file that is already in the game folder and was not installed by this tool is
#   never overwritten (SilentPatch, the trainer, VC Modern Fix, ddraw.dll, ...).
# - Disable moves a mod's files into VCModManager\disabled\<mod>; Enable moves them
#   back. Remove deletes them (and the cached download). Install downloads the
#   latest release again and installs it.
# - A mod's .ini files are kept when it is updated, so your settings stay.
#
# Command line (optional): VCModManager.bat <action> [mod]
#   actions: status, check, install, update, enable, disable, remove, setup
#   mods:    widescreen, project2dfx, skygfx, d3d8to9, dxvk, asiloader (or all)
param(
    [string]$Action = '',
    [string]$Mod = '',
    [string]$GameDir = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $GameDir) { $GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$GameDir = (Resolve-Path $GameDir).Path
$Home_ = Join-Path $GameDir 'VCModManager'
$Downloads = Join-Path $Home_ 'downloads'
$DisabledDir = Join-Path $Home_ 'disabled'
$StateFile = Join-Path $Home_ 'state.json'
$LogFile = Join-Path $Home_ 'VCModManager.log'

# --------------------------------------------------------------------- the mods
# files: from = path inside the archive (after its single top folder, if any; a
# trailing / copies that folder), to = path in the game folder.
$Catalogue = @(
    [ordered]@{
        id = 'widescreen'; name = 'Widescreen Fix'; author = 'ThirteenAG'
        desc = 'Correct 16:9 / 21:9 picture, HUD, radar and menus (the game stretches 4:3).'
        repo = 'ThirteenAG/WidescreenFixesPack'; tag = 'gtavc'; asset = '^GTAVC\.WidescreenFix\.zip$'
        recommended = $false; needsLoader = $true; requires = @()
        retired = 'its current release is a whole overhaul (frame limits, menus, HUD, mouse, loading) that fights VC Modern Fix; VC Modern Fix gets its own widescreen support instead'
        files = @(
            @{ from = 'scripts/GTAVC.WidescreenFix.asi'; to = 'scripts\GTAVC.WidescreenFix.asi' },
            @{ from = 'scripts/GTAVC.WidescreenFix.ini'; to = 'scripts\GTAVC.WidescreenFix.ini' })
        # Set on every install, update and enable (the rest of the file is yours):
        # VC Modern Fix paces the frames (unlocked, its FPSLimit) and does the island and
        # interior loading its own way. With MenuFrameLimit at 60 the menus ran at 60
        # and the game's own 30 FPS cap came back in play.
        ini = @(@{ file = 'scripts\GTAVC.WidescreenFix.ini'; set = @{ MenuFrameLimit = '0'; NoIslandLoading = '0'; NoInteriorLoading = '0' } })
        skipped = 'd3d8.dll (its own ASI loader, would replace VC Modern Fix), scripts\global.ini (would stop .asi files in the game folder loading)'
    },
    [ordered]@{
        id = 'project2dfx'; name = 'Project2DFX'; author = 'ThirteenAG'
        desc = 'Distant street lights and lamp glows at night (LOD lights), plus its limit adjuster.'
        repo = 'ThirteenAG/III.VC.SA.IV.Project2DFX'; tag = 'gtavc'; asset = '^VC\.Project2DFX\.zip$'
        recommended = $false; needsLoader = $true; requires = @()
        retired = 'its draw distance and limit adjuster overlap VC Modern Fix (stacked distances, resized render lists, a crash in bullet traces); VC Modern Fix gets its own distant lights instead'
        files = @(
            @{ from = 'VCLodLights.asi'; to = 'VCLodLights.asi' },
            @{ from = 'VCLodLights.dat'; to = 'VCLodLights.dat' },
            @{ from = 'III.VC.SA.LimitAdjuster.asi'; to = 'III.VC.SA.LimitAdjuster.asi' },
            @{ from = 'III.VC.SA.LimitAdjuster.ini'; to = 'III.VC.SA.LimitAdjuster.ini' })
        # its log says which limits it left to VC Modern Fix
        ini = @(@{ file = 'III.VC.SA.LimitAdjuster.ini'; set = @{ Log = '1' } })
        skipped = ''
    },
    [ordered]@{
        id = 'd3d8to9'; name = 'd3d8to9'; author = 'crosire'
        desc = 'Runs the game on Direct3D 9 (needed by SkyGfx and DXVK). Chained behind VC Modern Fix as d3d8_next.dll.'
        repo = 'crosire/d3d8to9'; tag = 'latest'; asset = '^d3d8\.dll$'; single = $true
        recommended = $false; needsLoader = $false; requires = @()
        files = @(@{ from = 'd3d8.dll'; to = 'd3d8_next.dll' })
        chain = $true
        ini = @(); skipped = ''
    },
    [ordered]@{
        id = 'dxvk'; name = 'DXVK'; author = 'doitsujin'
        desc = 'Direct3D 9 on Vulkan: often smoother frame times on modern cards. Needs d3d8to9.'
        repo = 'doitsujin/dxvk'; tag = 'latest'; asset = '^dxvk-[0-9.]+\.tar\.gz$'
        recommended = $false; needsLoader = $false; requires = @('d3d8to9')
        files = @(@{ from = 'x32/d3d9.dll'; to = 'd3d9.dll' })
        ini = @(); skipped = 'its d3d8.dll (d3d8to9 is used instead, so SkyGfx works too) and the 64-bit files'
    },
    [ordered]@{
        id = 'skygfx'; name = 'SkyGfx'; author = 'aap'
        desc = 'PS2 / Xbox look: colour filter, trails, car and world reflections. Optional. Needs d3d8to9.'
        repo = 'aap/skygfx_vc'; tag = 'latest'; asset = '^SkyGfx_III_VC_.*\.zip$'
        recommended = $false; needsLoader = $true; requires = @('d3d8to9')
        files = @(
            @{ from = 'skygfx.asi'; to = 'skygfx.asi' },
            @{ from = 'VC/skygfx.ini'; to = 'skygfx.ini' },
            @{ from = 'VC/neo/'; to = 'neo\' },
            @{ from = 'rwd3d9.dll'; to = 'rwd3d9.dll' })
        ini = @(); skipped = 'd3d8.dll (d3d8to9, installed separately as d3d8_next.dll), skygfx.dll (same file as skygfx.asi), the III files'
    },
    [ordered]@{
        id = 'asiloader'; name = 'Ultimate ASI Loader'; author = 'ThirteenAG'
        desc = 'Loads .asi mods. Installed as dinput8.dll only when the game folder has no ASI loader.'
        repo = 'ThirteenAG/Ultimate-ASI-Loader'; tag = 'latest'; asset = '^Ultimate-ASI-Loader\.zip$'
        recommended = $false; needsLoader = $false; requires = @(); hidden = $true
        files = @(@{ from = '*.dll'; to = 'dinput8.dll' })
        ini = @(); skipped = ''
    }
)
# Files an ASI loader can be loaded as. If one exists, the game already has a loader.
$LoaderNames = @('dinput8.dll', 'dsound.dll', 'winmm.dll', 'version.dll', 'wininet.dll', 'xlive.dll', 'vorbisFile.dll')
# Never written, moved or deleted, whatever a mod's archive holds.
$Protected = @('gta-vc.exe', 'd3d8.dll', 'ddraw.dll', 'Mss32.dll', 'SilentPatchVC.asi', 'SilentPatchVC.ini',
               'VCTrainer.asi', 'VCTrainer.ini', 'VCModernFix.ini')

# --------------------------------------------------------------------- helpers
function Log([string]$msg) {
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    try { Add-Content -Path $LogFile -Value $line -Encoding UTF8 } catch {}
}
function Say([string]$msg, [string]$color = 'Gray') { Write-Host $msg -ForegroundColor $color; Log $msg }

function Get-ModDef([string]$id) { $Catalogue | Where-Object { $_.id -eq $id } | Select-Object -First 1 }

function Load-State {
    if (Test-Path $StateFile) {
        $raw = Get-Content $StateFile -Raw -Encoding UTF8 | ConvertFrom-Json
        $s = @{ asked = [bool]$raw.asked; mods = @{} }
        if ($raw.mods) {
            foreach ($p in $raw.mods.PSObject.Properties) {
                $m = $p.Value
                $s.mods[$p.Name] = @{ status = $m.status; tag = $m.tag; assetName = $m.assetName; assetUpdated = $m.assetUpdated
                                      installed = $m.installed; files = $(if ($m.files) { @($m.files) } else { @() }) }
            }
        }
        return $s
    }
    return @{ asked = $false; mods = @{} }
}
function Save-State($s) {
    New-Item -ItemType Directory -Force $Home_ | Out-Null
    ($s | ConvertTo-Json -Depth 6) | Set-Content -Path $StateFile -Encoding UTF8
}

function Assert-GameClosed {
    if (Get-Process -Name 'gta-vc' -ErrorAction SilentlyContinue) {
        throw 'GTA Vice City is running. Close the game first, then run this again.'
    }
}

# GitHub answers 60 unauthenticated API calls an hour: each release is asked once per run.
$ReleaseCache = @{}
function Get-Release($def) {
    if ($ReleaseCache.ContainsKey($def.id)) { return $ReleaseCache[$def.id] }
    $h = @{ 'User-Agent' = 'VCModManager'; 'Accept' = 'application/vnd.github+json' }
    $url = if ($def.tag -eq 'latest') { "https://api.github.com/repos/$($def.repo)/releases/latest" }
           else { "https://api.github.com/repos/$($def.repo)/releases/tags/$($def.tag)" }
    $rel = Invoke-RestMethod -Uri $url -Headers $h
    $asset = $rel.assets | Where-Object { $_.name -match $def.asset } | Select-Object -First 1
    if (-not $asset) { throw "No file matching $($def.asset) in $($def.repo) release $($rel.tag_name)." }
    $r = @{ tag = [string]$rel.tag_name; name = [string]$asset.name; url = [string]$asset.browser_download_url
            updated = [string]$asset.updated_at; size = [int64]$asset.size }
    $ReleaseCache[$def.id] = $r
    return $r
}

function Has-Loader {
    foreach ($n in $LoaderNames) { if (Test-Path (Join-Path $GameDir $n)) { return $true } }
    return $false
}

function Owned-By($s, [string]$rel) {
    foreach ($k in $s.mods.Keys) { if ($s.mods[$k].files -contains $rel) { return $k } }
    return $null
}

function Set-IniValues([string]$path, $values) {
    if (-not (Test-Path $path)) { return }
    $lines = Get-Content $path -Encoding UTF8
    foreach ($key in $values.Keys) {
        $done = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) {
                $comment = ''
                if ($lines[$i] -match '(\s+;.*)$') { $comment = $Matches[1] }
                $lines[$i] = ($lines[$i] -replace '=\s*[^;]*', ('= ' + $values[$key] + ' ')).TrimEnd()
                if ($comment -and -not $lines[$i].Contains($comment.Trim())) { $lines[$i] += $comment }
                $done = $true
            }
        }
        if (-not $done) { $lines += "$key = $($values[$key])" }
    }
    Set-Content -Path $path -Value $lines -Encoding UTF8
}

# The settings a mod needs to work next to VC Modern Fix (the catalogue's ini list).
function Apply-ModSettings($def) {
    foreach ($t in $def.ini) {
        $path = Join-Path $GameDir $t.file
        if (-not (Test-Path $path)) { continue }
        Set-IniValues $path $t.set
        Say ('  {0}: {1}' -f $t.file, (($t.set.GetEnumerator() | ForEach-Object { "$($_.Key) = $($_.Value)" }) -join ', '))
    }
}

# VC Modern Fix passes graphics on to the file named by D3D8Chain in VCModernFix.ini.
function Set-Chain([bool]$on) {
    $ini = Join-Path $GameDir 'VCModernFix.ini'
    if (-not (Test-Path $ini)) { Say '  VCModernFix.ini not found; set D3D8Chain=d3d8_next.dll yourself.' Yellow; return }
    $lines = Get-Content $ini
    $changed = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*D3D8Chain\s*=(.*)$') {
            $cur = $Matches[1].Trim()
            if ($on -and $cur -ne 'd3d8_next.dll') {
                if ($cur) { Say "  D3D8Chain was '$cur'; left as it is (another wrapper is chained)." Yellow; return }
                $lines[$i] = 'D3D8Chain=d3d8_next.dll'; $changed = $true
            } elseif (-not $on -and $cur -eq 'd3d8_next.dll') {
                $lines[$i] = 'D3D8Chain='; $changed = $true
            }
        }
    }
    if ($changed) {
        Set-Content -Path $ini -Value $lines
        Say ('  VCModernFix.ini: D3D8Chain=' + $(if ($on) { 'd3d8_next.dll' } else { '(empty)' }))
    }
}

# Extract the download into a fresh folder; returns the folder whose content the
# file list refers to (the archive's single top folder, if it has one).
function Expand-Download([string]$file, $def) {
    $stage = Join-Path $Home_ ('stage-' + $def.id)
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    if ($def.single) { Copy-Item $file (Join-Path $stage ([IO.Path]::GetFileName($file))); return $stage }
    if ($file -like '*.zip') { Expand-Archive -Path $file -DestinationPath $stage -Force }
    elseif ($file -like '*.tar.gz' -or $file -like '*.tgz') { & tar.exe -xzf $file -C $stage; if ($LASTEXITCODE) { throw "tar could not extract $file" } }
    else { throw "Unknown archive type: $file" }
    $top = @(Get-ChildItem $stage)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) { return $top[0].FullName }
    return $stage
}

# The (source, target) pairs of a mod's file list inside an extracted archive.
function Resolve-Files([string]$root, $def) {
    $pairs = @()
    foreach ($f in $def.files) {
        $from = $f.from -replace '/', '\'
        if ($from.EndsWith('\')) {
            $dir = Join-Path $root $from.TrimEnd('\')
            if (-not (Test-Path $dir)) { throw "The archive has no folder $($f.from); its layout changed." }
            foreach ($item in Get-ChildItem $dir -Recurse -File) {
                $rel = $item.FullName.Substring($dir.Length).TrimStart('\')
                $pairs += @{ src = $item.FullName; dst = ($f.to.TrimEnd('\') + '\' + $rel) }
            }
        } elseif ($from.Contains('*')) {
            $hit = Get-ChildItem $root -Recurse -File -Filter ([IO.Path]::GetFileName($from)) | Select-Object -First 1
            if (-not $hit) { throw "The archive has no file matching $($f.from)." }
            $pairs += @{ src = $hit.FullName; dst = $f.to }
        } else {
            $src = Join-Path $root $from
            if (-not (Test-Path $src)) { throw "The archive has no $($f.from); its layout changed." }
            $pairs += @{ src = $src; dst = $f.to }
        }
    }
    return $pairs
}

# --------------------------------------------------------------------- actions
function Install-Mod($s, [string]$id, [bool]$isUpdate = $false) {
    $def = Get-ModDef $id
    if ($def.retired) {
        throw "$($def.name) is no longer offered: $($def.retired). If it is installed, you can disable or remove it."
    }
    foreach ($r in $def.requires) {
        $st = $s.mods[$r]
        if (-not $st) { Say "  $($def.name) needs $((Get-ModDef $r).name); installing it first."; Install-Mod $s $r }
        elseif ($st.status -eq 'disabled') { Say "  $($def.name) needs $((Get-ModDef $r).name); enabling it."; Enable-Mod $s $r }
    }
    if ($def.needsLoader -and -not (Has-Loader)) {
        Say '  No ASI loader in the game folder; installing Ultimate ASI Loader as dinput8.dll.'
        Install-Mod $s 'asiloader'
    }
    if ($id -eq 'asiloader' -and (Has-Loader) -and -not $s.mods['asiloader']) {
        Say '  The game folder already has an ASI loader; nothing to install.'; return
    }
    $old = $s.mods[$id]
    if ($old -and $old.status -eq 'disabled') { Enable-Mod $s $id }

    Say "Fetching the latest $($def.name) from github.com/$($def.repo) ..." Cyan
    $rel = Get-Release $def
    New-Item -ItemType Directory -Force $Downloads | Out-Null
    $file = Join-Path $Downloads $rel.name
    # a cached download of the same release file is used again (same size, and the same upload date as installed)
    $have = (Test-Path $file) -and (Get-Item $file).Length -eq $rel.size -and (-not $old -or $old.assetUpdated -eq $rel.updated)
    if ($have) {
        Say "  Using the downloaded $($rel.name) ($([math]::Round($rel.size / 1MB, 1)) MB)."
    } else {
        Say "  Downloading $($rel.name) ($([math]::Round($rel.size / 1MB, 1)) MB, release $($rel.tag), updated $($rel.updated.Substring(0,10))) ..."
        Invoke-WebRequest -Uri $rel.url -OutFile $file -Headers @{ 'User-Agent' = 'VCModManager' }
        if ((Get-Item $file).Length -ne $rel.size) { throw "The download of $($rel.name) is incomplete." }
    }
    $root = Expand-Download $file $def
    $pairs = Resolve-Files $root $def

    $installed = @()
    foreach ($p in $pairs) {
        $dst = Join-Path $GameDir $p.dst
        $leaf = [IO.Path]::GetFileName($p.dst)
        if ($Protected -contains $leaf -and $p.dst -notlike '*\*') { Say "  Skipped $($p.dst): protected file." Yellow; continue }
        $owner = Owned-By $s $p.dst
        if ((Test-Path $dst) -and $owner -ne $id) {
            Say "  Skipped $($p.dst): already in the game folder and not installed by this tool$(if ($owner) { " (belongs to $owner)" })." Yellow
            continue
        }
        if ($isUpdate -and (Test-Path $dst) -and $dst -like '*.ini') { $installed += $p.dst; continue }   # keep your settings
        New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
        Copy-Item -Path $p.src -Destination $dst -Force
        $installed += $p.dst
    }
    Apply-ModSettings $def
    # files the old version had and the new one doesn't
    if ($old) {
        foreach ($f in $old.files) { if ($installed -notcontains $f) { $x = Join-Path $GameDir $f; if (Test-Path $x) { Remove-Item -Force $x } } }
    }
    Remove-Item -Recurse -Force (Join-Path $Home_ ('stage-' + $def.id))
    if (-not $installed.Count) {
        throw "$($def.name) was not installed: every file it needs is already in the game folder from something else (see above). Remove that file first."
    }
    $s.mods[$id] = @{ status = 'enabled'; tag = $rel.tag; assetName = $rel.name; assetUpdated = $rel.updated
                      installed = (Get-Date -Format 'yyyy-MM-dd'); files = $installed }
    if ($def.chain) { Set-Chain $true }
    Save-State $s
    Say ("  {0} {1}: {2} file(s)." -f $def.name, $(if ($isUpdate) { 'updated' } else { 'installed' }), $installed.Count) Green
    if ($def.skipped) { Say "  Not installed from its archive: $($def.skipped)." }
}

function Disable-Mod($s, [string]$id) {
    $def = Get-ModDef $id; $st = $s.mods[$id]
    if (-not $st) { Say "$($def.name) is not installed."; return }
    if ($st.status -eq 'disabled') { Say "$($def.name) is already disabled."; return }
    foreach ($k in @($s.mods.Keys)) {
        if ((Get-ModDef $k).requires -contains $id -and $s.mods[$k].status -eq 'enabled') {
            Say "  $((Get-ModDef $k).name) needs $($def.name); disabling it too."
            Disable-Mod $s $k
        }
    }
    foreach ($f in $st.files) {
        $src = Join-Path $GameDir $f
        $dst = Join-Path (Join-Path $DisabledDir $id) $f
        if (Test-Path $src) { New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null; Move-Item -Force $src $dst }
    }
    Remove-EmptyDirs $st.files
    $st.status = 'disabled'
    if ($def.chain) { Set-Chain $false }
    Save-State $s
    Say "$($def.name) disabled (files kept in VCModManager\disabled\$id)." Green
}

function Enable-Mod($s, [string]$id) {
    $def = Get-ModDef $id; $st = $s.mods[$id]
    if (-not $st) { Say "$($def.name) is not installed; installing it."; Install-Mod $s $id; return }
    if ($st.status -eq 'enabled') { Say "$($def.name) is already enabled."; return }
    foreach ($r in $def.requires) {
        if (-not $s.mods[$r]) { Install-Mod $s $r } elseif ($s.mods[$r].status -eq 'disabled') { Enable-Mod $s $r }
    }
    $missing = 0
    foreach ($f in $st.files) {
        $src = Join-Path (Join-Path $DisabledDir $id) $f
        $dst = Join-Path $GameDir $f
        if (-not (Test-Path $src)) { $missing++; continue }
        if (Test-Path $dst) { Say "  Skipped $f`: a file of that name is in the game folder now." Yellow; continue }
        New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
        Move-Item -Force $src $dst
    }
    $st.status = 'enabled'
    if ($def.chain) { Set-Chain $true }
    Apply-ModSettings $def
    Save-State $s
    if ($missing) { Say "  $missing file(s) were missing from the disabled copy; installing $($def.name) again."; Install-Mod $s $id $true; return }
    Say "$($def.name) enabled." Green
}

function Remove-Mod($s, [string]$id) {
    $def = Get-ModDef $id; $st = $s.mods[$id]
    if (-not $st) { Say "$($def.name) is not installed."; return }
    foreach ($k in @($s.mods.Keys)) {
        if ((Get-ModDef $k).requires -contains $id) {
            Say "  $((Get-ModDef $k).name) needs $($def.name); disabling it (remove it too if you like)."
            if ($s.mods[$k].status -eq 'enabled') { Disable-Mod $s $k }
        }
    }
    foreach ($f in $st.files) {
        foreach ($base in @($GameDir, (Join-Path $DisabledDir $id))) {
            $x = Join-Path $base $f
            if (Test-Path $x) { Remove-Item -Force $x }
        }
    }
    Remove-EmptyDirs $st.files
    $d = Join-Path $DisabledDir $id; if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    if ($st.assetName) { $c = Join-Path $Downloads $st.assetName; if (Test-Path $c) { Remove-Item -Force $c } }
    if ($def.chain) { Set-Chain $false }
    $s.mods.Remove($id)
    Save-State $s
    Say ("$($def.name) removed completely." + $(if ($def.retired) { '' } else { ' Install it again any time from this menu.' })) Green
}

# folders the mod made that are now empty (scripts\, neo\...)
function Remove-EmptyDirs($files) {
    $dirs = $files | ForEach-Object { Split-Path -Parent $_ } | Where-Object { $_ } | Sort-Object -Unique -Descending
    foreach ($d in $dirs) {
        $full = Join-Path $GameDir $d
        while ($full.Length -gt $GameDir.Length -and (Test-Path $full) -and -not (Get-ChildItem $full -Force)) {
            Remove-Item -Force $full
            $full = Split-Path -Parent $full
        }
    }
}

function Show-Status($s, [bool]$online) {
    Write-Host ''
    Write-Host '  #  Mod                  State          Installed release      Latest release' -ForegroundColor Cyan
    $i = 0
    foreach ($def in $Catalogue) {
        if (($def.hidden -or $def.retired) -and -not $s.mods[$def.id]) { continue }
        $i++
        $st = $s.mods[$def.id]
        $state = if ($st) { $st.status } else { 'not installed' }
        $have = if ($st) { '{0} ({1})' -f $st.tag, $st.assetUpdated.Substring(0, 10) } else { '-' }
        $latest = '-'
        if ($def.retired) { $latest = 'retired: remove recommended' }
        elseif ($online) {
            try { $r = Get-Release $def; $latest = '{0} ({1})' -f $r.tag, $r.updated.Substring(0, 10)
                  if ($st -and $r.updated -ne $st.assetUpdated) { $latest += '  UPDATE' } } catch { $latest = 'could not check' }
        }
        $color = if ($state -eq 'enabled') { 'Green' } elseif ($state -eq 'disabled') { 'Yellow' } else { 'Gray' }
        Write-Host ('  {0}  {1,-20} {2,-14} {3,-22} {4}' -f $i, $def.name, $state, $have, $latest) -ForegroundColor $color
    }
    Write-Host ''
}

function Visible-Mods($s) { @($Catalogue | Where-Object { (-not $_.hidden -and -not $_.retired) -or $s.mods[$_.id] }) }

function First-Run($s) {
    Write-Host ''
    Write-Host 'VC Modern Fix - optional enhancement mods' -ForegroundColor Cyan
    Write-Host 'These are other authors'' mods, downloaded from their official GitHub releases.'
    Write-Host 'You can disable, enable or remove each one later from this tool.'
    Write-Host ''
    $yn = Read-Host 'Do you want to install extra enhancing mods? (Y/N)'
    $s.asked = $true
    if ($yn -notmatch '^[Yy]') { Save-State $s; Say 'No extra mods installed. Run VCModManager.bat again any time to add them.'; return }
    foreach ($def in $Catalogue) {
        if ($def.hidden -or $def.retired) { continue }
        $tag =if ($def.recommended) { 'recommended' } else { 'optional' }
        Write-Host ''
        Write-Host ("{0} by {1} ({2})" -f $def.name, $def.author, $tag) -ForegroundColor White
        Write-Host ("  " + $def.desc)
        $default = if ($def.recommended) { 'Y' } else { 'N' }
        $a = Read-Host ("  Install it? (Y/N, Enter = {0})" -f $default)
        if (-not $a) { $a = $default }
        if ($a -match '^[Yy]') {
            try { Install-Mod $s $def.id } catch { Say "  $($def.name) failed: $($_.Exception.Message)" Red }
        }
    }
    Save-State $s
}

function Menu($s) {
    for (;;) {
        Show-Status $s $true
        Write-Host '  Pick a mod by number, or: U = update all installed, Q = quit'
        $c = Read-Host '  Choice'
        if ($c -match '^[Qq]') { return }
        if ($c -match '^[Uu]') { foreach ($k in @($s.mods.Keys)) { if ($s.mods[$k].status -eq 'enabled' -and -not (Get-ModDef $k).retired) { try { Install-Mod $s $k $true } catch { Say "  $k failed: $($_.Exception.Message)" Red } } }; continue }
        $list = Visible-Mods $s
        $n = 0
        if (-not [int]::TryParse($c, [ref]$n) -or $n -lt 1 -or $n -gt $list.Count) { continue }
        $def = $list[$n - 1]
        $st = $s.mods[$def.id]
        Write-Host ''
        Write-Host ("{0} by {1}: {2}" -f $def.name, $def.author, $def.desc) -ForegroundColor White
        if ($def.retired) { Write-Host "  No longer offered: $($def.retired)." -ForegroundColor Yellow }
        if (-not $st) { $opts = 'I = install' }
        elseif ($def.retired) { $opts = $(if ($st.status -eq 'enabled') { 'D = disable, R = remove completely' } else { 'E = enable, R = remove completely' }) }
        elseif ($st.status -eq 'enabled') { $opts = 'U = update, D = disable, R = remove completely' }
        else { $opts = 'E = enable, R = remove completely' }
        $a = Read-Host ("  $opts, Enter = back")
        try {
            switch -regex ($a) {
                '^[Ii]' { if (-not $st) { Install-Mod $s $def.id } }
                '^[Uu]' { if ($st) { Install-Mod $s $def.id $true } }
                '^[Dd]' { Disable-Mod $s $def.id }
                '^[Ee]' { Enable-Mod $s $def.id }
                '^[Rr]' { Remove-Mod $s $def.id }
            }
        } catch { Say "  Failed: $($_.Exception.Message)" Red }
    }
}

# --------------------------------------------------------------------- main
if (-not (Test-Path (Join-Path $GameDir 'gta-vc.exe'))) {
    Write-Host "gta-vc.exe is not in $GameDir. Put VCModManager.bat and VCModManager.ps1 next to gta-vc.exe." -ForegroundColor Red
    exit 1
}
New-Item -ItemType Directory -Force $Home_ | Out-Null
$state = Load-State
try {
    $ids = if ($Mod -eq 'all') { @($state.mods.Keys | Where-Object { $Action -notin @('install', 'update', 'enable') -or -not (Get-ModDef $_).retired }) }
           elseif ($Mod) { @($Mod) } else { @() }
    foreach ($id in $ids) { if (-not (Get-ModDef $id)) { throw "Unknown mod '$id'. Mods: $(($Catalogue | % id) -join ', ')" } }
    if ($Action -in @('install', 'update', 'enable', 'disable', 'remove') -and -not $ids.Count) {
        throw "Name a mod: VCModManager.bat $Action <mod>. Mods: $(($Catalogue | % id) -join ', '), or all."
    }
    switch ($Action) {
        ''        { Assert-GameClosed; if (-not $state.asked) { First-Run $state }; Menu $state }
        'setup'   { Assert-GameClosed; First-Run $state }
        'status'  { Show-Status $state $false }
        'check'   { Show-Status $state $true }
        'install' { Assert-GameClosed; foreach ($id in $ids) { Install-Mod $state $id } }
        'update'  { Assert-GameClosed; foreach ($id in $ids) { Install-Mod $state $id $true } }
        'enable'  { Assert-GameClosed; foreach ($id in $ids) { Enable-Mod $state $id } }
        'disable' { Assert-GameClosed; foreach ($id in $ids) { Disable-Mod $state $id } }
        'remove'  { Assert-GameClosed; foreach ($id in $ids) { Remove-Mod $state $id } }
        default   { throw "Unknown action '$Action'. Actions: status, check, install, update, enable, disable, remove, setup" }
    }
} catch {
    Say ("Error: " + $_.Exception.Message) Red
    exit 1
}
