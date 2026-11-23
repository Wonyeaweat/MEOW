"""Run tools with bounded timeout cleanup and error reporting."""
import os
import shutil
import signal
import subprocess
import tempfile
from pathlib import Path


class ProcessError(RuntimeError):
    pass


def prepare_work_directory(path, *, prefix):
    temporary = tempfile.TemporaryDirectory(prefix=prefix) if path is None else None
    work = Path(temporary.name if temporary else path).resolve()
    work.mkdir(parents=True, exist_ok=True)
    return work, temporary


def resolve_executable(command):
    path = Path(shutil.which(str(command)) or str(command))
    if path.is_file() and os.access(path, os.X_OK):
        return path.resolve()
    raise ProcessError(f'executable not found: {command}')


def run_process(command, *, timeout=None, check=True, env=None):
    argv = [str(item) for item in command]
    try:
        process = subprocess.Popen(argv, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=env, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(process.pid, sig)
                except ProcessLookupError:
                    pass
                try:
                    process.communicate(timeout=5)
                    break
                except subprocess.TimeoutExpired:
                    continue
            # A descendant may hold inherited pipes after leaving the process group.
            process.stdout.close()
            process.stderr.close()
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            raise ProcessError(f'{argv[0]} timed out after {timeout:g}s') from error
    except (OSError, subprocess.SubprocessError) as error:
        raise ProcessError(str(error)) from error
    if check and process.returncode:
        raise ProcessError(f'{argv[0]} exited with {process.returncode}:\n{stderr or stdout}')
    return subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)
