# Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later

param(
  [switch]$SkipBuild,
  [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrEmpty($OutputDir)) {
  $OutputDir = Join-Path $RepoRoot "build\package\amigaos3"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
  $OutputDir = Join-Path $RepoRoot $OutputDir
}

$PackageRoot = [System.IO.Path]::GetFullPath($OutputDir)
$SafeRoot = [System.IO.Path]::GetFullPath(
  (Join-Path $RepoRoot "build\package")
)
# Append a trailing separator so the containment check compares whole path
# components: a prefix-only check would accept siblings like
# build\package-backup and the Remove-Item below would erase them.
$SafeRootWithSep = $SafeRoot.TrimEnd('\') + '\'
if (-not $PackageRoot.StartsWith(
    $SafeRootWithSep, [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "OutputDir must stay under build/package"
}

if (-not $SkipBuild) {
  powershell -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot "build-m68k-amigaos.ps1")
}

if (Test-Path -LiteralPath $PackageRoot) {
  Remove-Item -LiteralPath $PackageRoot -Recurse -Force
}

$Dirs = @(
  "Libs",
  "Classes\DataTypes",
  "Storage\DataTypes",
  "C",
  "Developer\Include\zz9k",
  "Developer\Include\libraries",
  "Developer\Include\proto",
  "Developer\Include\clib",
  "Developer\Include\inline",
  "Developer\Include\pragmas",
  "Developer\Include\fd",
  "Developer\FD",
  "Docs",
  "Examples",
  "Archives"
)
foreach ($Dir in $Dirs) {
  New-Item -ItemType Directory -Force `
    -Path (Join-Path $PackageRoot $Dir) | Out-Null
}

function Copy-One {
  param([string]$SourceRel, [string]$DestRel)
  $Source = Join-Path $RepoRoot ($SourceRel -replace "/", "\")
  $Dest = Join-Path $PackageRoot ($DestRel -replace "/", "\")
  Copy-Item -LiteralPath $Source -Destination $Dest -Force
}

function Copy-Tree {
  param([string]$SourceRel, [string]$DestRel)
  $Source = Join-Path $RepoRoot ($SourceRel -replace "/", "\")
  $Dest = Join-Path $PackageRoot ($DestRel -replace "/", "\")
  Copy-Item -Path (Join-Path $Source "*") -Destination $Dest `
    -Recurse -Force
}

function Decode-Base64File {
  param([string]$SourceRel, [string]$DestRel)
  $Source = Join-Path $RepoRoot ($SourceRel -replace "/", "\")
  $Dest = Join-Path $PackageRoot ($DestRel -replace "/", "\")
  $Text = Get-Content -LiteralPath $Source -Raw
  $Bytes = [System.Convert]::FromBase64String($Text)
  [System.IO.File]::WriteAllBytes($Dest, $Bytes)
}

function Make-Guide {
  param([string]$SourceRel, [string]$Name)
  # Amiga-side manuals ship as AmiGuide databases generated from the
  # canonical Markdown sources: raw .md has no Amiga reader. Sibling
  # names must match $GuideSpecs so cross-document links resolve.
  & python "$RepoRoot\scripts\md2guide.py" --name $Name @GuideSiblings `
    "$RepoRoot\$($SourceRel -replace '/', '\')" `
    (Join-Path $PackageRoot "Docs\$Name.guide")
  if ($LASTEXITCODE -ne 0) { exit 1 }
}

Copy-One "build/zz9k.library" "Libs/zz9k.library"
Copy-One "build/mpega.library" "Libs/mpega.library"

Copy-One "build/zz9k-info" "C/zz9k-info"
Copy-One "build/zz9k-services" "C/zz9k-services"
Copy-One "build/zz9k-fabriclease" "C/zz9k-fabriclease"
Copy-One "build/zz9k-bench" "C/zz9k-bench"
Copy-One "build/zz9k-hash" "C/zz9k-hash"
Copy-One "build/zz9k-inflate" "C/zz9k-inflate"
Copy-One "build/zz9k-archive" "C/zz9k-archive"
Copy-One "build/zz9k-irqtest" "C/zz9k-irqtest"
Copy-One "build/zz9k-chacha" "C/zz9k-chacha"
Copy-One "build/zz9k-aead" "C/zz9k-aead"
Copy-One "build/zz9k-cryptobench" "C/zz9k-cryptobench"
Copy-One "build/zz9k-m68kbench" "C/zz9k-m68kbench"
Copy-One "build/zz9k-mp3" "C/zz9k-mp3"
Copy-One "build/zzplay" "C/ZZPlay"
Copy-One "amiga/icons/zzplay.info" "C/ZZPlay.info"
Copy-One "amiga/icons/zzplay-project.info" "Docs/ZZPlay-project.info"
Copy-One "build/zz9k-mpega-smoke" "C/zz9k-mpega-smoke"
Copy-One "build/zz9k-jpeg" "C/zz9k-jpeg"
Copy-One "build/zz9k-png" "C/zz9k-png"
Copy-One "build/zz9k-view" "C/zz9k-view"
Copy-One "build/zz9k-dtprobe" "C/zz9k-dtprobe"
Copy-One "build/zz9k-picture.datatype" `
  "Classes/DataTypes/zz9k-picture.datatype"
Copy-One "build/zz9k-sound.datatype" `
  "Classes/DataTypes/zz9k-sound.datatype"
$DescriptorDirectory = Join-Path $RepoRoot "amiga/datatypes/descriptors"
& python "$RepoRoot\scripts\generate-datatype-descriptors.py" --source-dir $DescriptorDirectory --output-dir (Join-Path $PackageRoot "Storage/DataTypes") --code-dir (Join-Path $RepoRoot "build/dtcode")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Get-ChildItem -LiteralPath $DescriptorDirectory -Filter *.dtid -File |
  Sort-Object Name |
  ForEach-Object {
    Copy-One ("amiga/datatypes/descriptors/" + $_.BaseName + ".info") ("Storage/DataTypes/" + $_.BaseName + ".info")
  }
Copy-One "build/zz9k-smoke" "C/zz9k-smoke"
Copy-One "build/zz9k-surface-info" "C/zz9k-surface-info"
Copy-One "build/zz9k-palette" "C/zz9k-palette"
Copy-One "build/zz9k-fbtest" "C/zz9k-fbtest"
Copy-One "build/zz9k-scaletest" "C/zz9k-scaletest"
Copy-One "build/zz9k-surfaceops" "C/zz9k-surfaceops"
Copy-One "build/zz9k-probe" "C/zz9k-probe"
Copy-One "build/zz9k-step" "C/zz9k-step"
Copy-One "build/zz9k-libtest" "C/zz9k-libtest"
Copy-One "build/zz9k-libasync" "C/zz9k-libasync"
Copy-One "build/zz9k-libbatch" "C/zz9k-libbatch"
Copy-One "build/zz9k-libcancel" "C/zz9k-libcancel"
Copy-One "build/zz9k-libwait" "C/zz9k-libwait"
Copy-One "build/zz9k-libwaitbatch" "C/zz9k-libwaitbatch"
Copy-One "build/zz9k-libevent" "C/zz9k-libevent"
Copy-One "build/zz9k-libeventbatch" "C/zz9k-libeventbatch"
Copy-One "build/zz9k-libeventcb" "C/zz9k-libeventcb"
Copy-One "build/zz9k-libeventlife" "C/zz9k-libeventlife"
Copy-One "build/zz9k-libeventports" "C/zz9k-libeventports"
Copy-One "build/zz9k-libeventcancel" "C/zz9k-libeventcancel"
Copy-One "build/zz9k-libdecode" "C/zz9k-libdecode"
Copy-One "build/zz9k-libsmoke" "C/zz9k-libsmoke"
Copy-One "build/zz9k-library-demo" "C/zz9k-library-demo"
Copy-One "build/zz9k-jpeg-stream-demo" "C/zz9k-jpeg-stream-demo"
Copy-One "build/zz9k-typed-decode-demo" "C/zz9k-typed-decode-demo"
Copy-One "build/zz9k-crypto-demo" "C/zz9k-crypto-demo"

Copy-Tree "include/zz9k" "Developer/Include/zz9k"
Copy-Tree "host/include/zz9k" "Developer/Include/zz9k"
Copy-Tree "amiga/include/zz9k" "Developer/Include/zz9k"
Copy-Tree "amiga/include/libraries" "Developer/Include/libraries"
Copy-Tree "amiga/include/proto" "Developer/Include/proto"
Copy-Tree "amiga/include/clib" "Developer/Include/clib"
Copy-Tree "amiga/include/inline" "Developer/Include/inline"
Copy-Tree "amiga/include/pragmas" "Developer/Include/pragmas"
Copy-One "amiga/fd/zz9k_lib.fd" "Developer/FD/zz9k_lib.fd"
Copy-One "amiga/fd/mpega_lib.fd" "Developer/FD/mpega_lib.fd"
Copy-One "amiga/fd/mpega.fd" "Developer/FD/mpega.fd"
Copy-One "amiga/fd/mpega.fd" "Developer/Include/fd/mpega.fd"

$GuideSpecs = @(
  "README.md:ZZ9000-SDK",
  "docs/zz9k-library.md:zz9k-library",
  "docs/zz9k-zorro2-services.md:zz9k-zorro2-services",
  "docs/zz9k-picture-datatype.md:zz9k-picture-datatype",
  "docs/zz9k-sound-datatype.md:zz9k-sound-datatype",
  "docs/zzplay.md:ZZPlay",
  "docs/zz9k-amissl-provider.md:zz9k-amissl-provider"
)
$GuideSiblings = @()
foreach ($spec in $GuideSpecs) {
  $GuideSiblings += "--sibling"
  $GuideSiblings += "$($spec.Split(':')[1]).guide"
}
foreach ($spec in $GuideSpecs) {
  $parts = $spec.Split(':')
  Make-Guide $parts[0] $parts[1]
}

Decode-Base64File "tests/fixtures/archives/split-deflate.7z.b64" `
  "Archives/split-deflate.7z"
Decode-Base64File "tests/fixtures/archives/split-lzma.7z.b64" `
  "Archives/split-lzma.7z"
Decode-Base64File "tests/fixtures/archives/split-lzma2.7z.b64" `
  "Archives/split-lzma2.7z"
Copy-One "examples/amiga-library/zz9k-library-demo.c" `
  "Examples/zz9k-library-demo.c"
Copy-One "examples/amiga-jpeg-stream/zz9k-jpeg-stream-demo.c" `
  "Examples/zz9k-jpeg-stream-demo.c"
Copy-One "examples/amiga-typed-decode/zz9k-typed-decode-demo.c" `
  "Examples/zz9k-typed-decode-demo.c"
Copy-One "examples/amiga-crypto/zz9k-crypto-demo.c" `
  "Examples/zz9k-crypto-demo.c"

$Manifest = Join-Path $PackageRoot "MANIFEST.sha256"
Get-ChildItem -LiteralPath $PackageRoot -Recurse -File |
  Where-Object { $_.FullName -ne $Manifest } |
  Sort-Object FullName |
  ForEach-Object {
    $Rel = $_.FullName.Substring($PackageRoot.Length + 1)
    $Rel = $Rel -replace "\\", "/"
    $Hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
    "{0}  {1}" -f $Hash.Hash.ToLowerInvariant(), $Rel
  } |
  Set-Content -LiteralPath $Manifest -Encoding ASCII

Write-Host "Packaged AmigaOS 3 SDK to $PackageRoot"
