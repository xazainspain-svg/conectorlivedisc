# Configura todo en Windows: Node, plugin VST3 (desde GitHub Actions), bridge y .env. Ejecutar en PowerShell como Administrador.
$ErrorActionPreference = 'Stop'
$Repo   = 'xazainspain-svg/conectorlivedisc'
$Branch = 'claude/wizardly-keller-oymrnj'
$Dir    = Join-Path $HOME 'conectorlivedisc'

function Refresh-Path { $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') }
function Ensure($cmd, $wingetId) {
  if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
    Write-Host "Instalando $wingetId ..." -ForegroundColor Cyan
    winget install --id $wingetId -e --silent --accept-package-agreements --accept-source-agreements
    Refresh-Path
  }
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Abre PowerShell como Administrador (clic derecho > Ejecutar como administrador) y vuelve a pegar el comando.'
}

Ensure git    'Git.Git'
Ensure node   'OpenJS.NodeJS.LTS'
Ensure gh     'GitHub.cli'

& gh auth status 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host 'Inicia sesión en GitHub (se abrirá el navegador)...' -ForegroundColor Cyan; & gh auth login -w -h github.com }

if (Test-Path "$Dir\.git") { git -C $Dir fetch origin $Branch; git -C $Dir checkout $Branch; git -C $Dir pull origin $Branch }
else { gh repo clone $Repo $Dir -- -b $Branch }

Write-Host 'Instalando dependencias del bridge...' -ForegroundColor Cyan
Push-Location "$Dir\bridge"; npm install --no-audit --no-fund; Pop-Location

Write-Host 'Descargando el plugin VST3 compilado...' -ForegroundColor Cyan
$runId = gh run list -R $Repo --workflow build-plugin.yml --branch $Branch --status success --limit 1 --json databaseId --jq '.[0].databaseId'
if (-not $runId) { throw 'No hay ninguna compilación correcta en GitHub Actions todavía.' }
$tmp = Join-Path $env:TEMP 'ldsc-vst3'
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
gh run download $runId -R $Repo -n LiveDiscordSend-vst3-windows -D $tmp
$vst3Dir = Join-Path $env:CommonProgramFiles 'VST3'
New-Item -ItemType Directory -Force $vst3Dir | Out-Null
Get-ChildItem $tmp -Filter '*.vst3' | ForEach-Object { Copy-Item $_.FullName $vst3Dir -Recurse -Force }
Write-Host "Plugin copiado a $vst3Dir" -ForegroundColor Green

$envFile = "$Dir\bridge\.env"
if (-not (Test-Path $envFile)) {
  Write-Host "`nDatos del bot de Discord (discord.com/developers > tu app > Bot):" -ForegroundColor Cyan
  $token = Read-Host 'DISCORD_TOKEN'
  $guild = Read-Host 'GUILD_ID (ID del servidor)'
  $chan  = Read-Host 'VOICE_CHANNEL_ID (ID del canal de voz)'
  Copy-Item "$Dir\bridge\.env.example" $envFile
  (Get-Content $envFile) -replace '^DISCORD_TOKEN=.*', "DISCORD_TOKEN=$token" `
                         -replace '^GUILD_ID=.*', "GUILD_ID=$guild" `
                         -replace '^VOICE_CHANNEL_ID=.*', "VOICE_CHANNEL_ID=$chan" | Set-Content $envFile
}

Write-Host "`nListo. En Ableton: Preferencias > Plug-ins > Rescan, y pon 'Live Discord Send' al final del Master." -ForegroundColor Green
Write-Host 'Arrancando el bridge (Ctrl+C para parar; la próxima vez usa bridge\start.bat)...' -ForegroundColor Green
Set-Location "$Dir\bridge"
node --env-file=.env src/index.js
