Push-Location (Join-Path $PSScriptRoot 'laptop_host')
try {
    git pull
    if ($LASTEXITCODE -ne 0) {
        throw "git pull failed with exit code $LASTEXITCODE."
    }

    dotnet build .\X3LaptopCompanion.csproj
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    dotnet .\bin\Debug\net10.0-windows10.0.19041.0\X3LaptopCompanion.dll
    if ($LASTEXITCODE -ne 0) {
        throw "Laptop Companion exited with code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}