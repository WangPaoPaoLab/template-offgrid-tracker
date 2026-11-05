"""Replace Seeed's unchecked RRAM load with verified OpenOCD upload."""
Import('env')

from pathlib import Path

openocd = Path(env.PioPlatform().get_package_dir('tool-openocd')) / 'bin' / 'openocd'
command = ('"$PYTHONEXE" "$PROJECT_DIR/scripts/nrf54_openocd.py" '
           'flash "$SOURCE" --openocd "' + str(openocd) + '"')
if env.subst('$UPLOAD_PORT').strip() not in ('', '$UPLOAD_PORT'):
    command += ' --serial "$UPLOAD_PORT"'
env.Replace(UPLOADCMD=command)
