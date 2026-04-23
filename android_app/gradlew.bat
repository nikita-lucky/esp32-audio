@echo off
set DIR=%~dp0
set WRAPPER_JAR=%DIR%gradle\wrapper\gradle-wrapper.jar
set WRAPPER_URL=https://raw.githubusercontent.com/gradle/gradle/v8.5.0/gradle/wrapper/gradle-wrapper.jar

if not exist "%WRAPPER_JAR%" (
    echo Downloading gradle-wrapper.jar...
    if not exist "%DIR%gradle\wrapper" mkdir "%DIR%gradle\wrapper"
    powershell -Command "Invoke-WebRequest -Uri '%WRAPPER_URL%' -OutFile '%WRAPPER_JAR%'"
)

java -jar "%WRAPPER_JAR%" %*
