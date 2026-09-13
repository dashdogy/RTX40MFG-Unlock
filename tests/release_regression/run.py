"""Run the release harnesses in isolated directories with an exact DLL.

This does not launch or install into games. Windows presentation cases briefly
create test windows; Vulkan requires a compatible NVIDIA Vulkan device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dll', type=Path, required=True)
    parser.add_argument('--harness-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--provider', type=Path)
    parser.add_argument('--only', nargs='*')
    args = parser.parse_args()
    dll = args.dll.resolve(strict=True)
    harness = args.harness_dir.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = [(f'default-{mode}', 'StartupDefaultsTests', mode, 'winmm.dll')
             for mode in ('clean', 'legacy', 'fixed', 'dynamic', 'follow', 'environment')]
    cases += [('loader-version', 'ReleaseLoadCheck', 'loader', 'version.dll'),
              ('loader-winmm', 'ReleaseLoadCheck', 'loader', 'winmm.dll'),
              ('control', 'ControlResolverTests', 'control', 'winmm.dll'),
              ('policy', 'NvidiaPolicyTests', '', 'winmm.dll'),
              ('dynamic-availability', 'DynamicAvailabilityTests', '', 'winmm.dll'),
              ('first-launch', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('startup-menu', 'WarpPresentHarness', 'startup-menu', 'winmm.dll'),
              ('layered-menu', 'WarpPresentHarness', 'layered-slinit-menu', 'winmm.dll')]
    cases += [(name, executable, 'vulkan', 'winmm.dll') for name, executable in (
        ('vulkan-direct', 'VulkanMenuTests'), ('vulkan-dynamic', 'VulkanDynamicMenuTests'),
        ('vulkan-streamline', 'VulkanStreamlineMenuTests'), ('vulkan-mixed', 'VulkanMixedMenuTests'))]
    if args.provider:
        cases.append(('provider-gate', 'ProviderGateTests', 'provider', 'winmm.dll'))
    if args.only:
        unknown = set(args.only) - {case[0] for case in cases}
        if unknown:
            parser.error(f'Unknown or unavailable cases: {sorted(unknown)}')
        cases = [case for case in cases if case[0] in args.only]
    rows = []
    for name, executable, mode, proxy in cases:
        destination = Path(tempfile.mkdtemp(prefix=name+'-', dir=output))
        for filename in (executable+'.exe', 'sl.interposer.dll',
                         'ControlWrapperFixture.dll', 'ExistingOverlayFixture.dll'):
            shutil.copy2(harness/filename, destination/filename)
        candidate = destination/proxy
        shutil.copy2(dll, candidate)
        if name != 'first-launch':
            (destination/'RTX40MFG-UI.ini').write_text(
                '[Overlay]\nFirstLaunchMenuShown=1\n', encoding='utf-8')
        provider_hash = sha(args.provider) if mode == 'provider' else None
        command = [str(destination/(executable+'.exe'))]
        if mode == 'vulkan':
            command += [str(candidate), str(destination)]
        elif mode == 'provider':
            command += [str(args.provider.resolve()), 'dll', str(candidate)]
        elif mode:
            command += [mode, str(candidate)]
        environment = dict(os.environ)
        for key in ('RTXMFG_SLOT_FAULT', 'RTX_MFG_ACTIVE_MULTIPLIER',
                    'RTX_MFG_CONFIG_PATH', 'RTX_MFG_STATUS_PATH'):
            environment.pop(key, None)
        def run(label, command):
            with (destination/(label+'.stdout.txt')).open('wb') as stdout, \
                 (destination/(label+'.stderr.txt')).open('wb') as stderr:
                process = subprocess.Popen(command, cwd=destination, env=environment,
                    stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    code = process.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    code = -1
            text = (destination/(label+'.stdout.txt')).read_text(encoding='utf-8', errors='replace')
            logfile = Path(tempfile.gettempdir())/f'RTXMFG-{process.pid}.log'
            if logfile.is_file():
                shutil.copy2(logfile, destination/(label+'.runtime.log'))
            row = dict(name=label, pid=process.pid, exit=code,
                       passed=code == 0 and not any(line.startswith('FAIL ') for line in text.splitlines()),
                       assertions=sum(line.startswith('PASS ') for line in text.splitlines()),
                       dllSHA256=sha(candidate), harnessSHA256=sha(Path(command[0])), directory=str(destination))
            if provider_hash:
                row['providerSHA256'] = provider_hash
                row['providerUnchanged'] = sha(args.provider) == provider_hash
                row['passed'] &= row['providerUnchanged']
            rows.append(row)
            print(json.dumps(row), flush=True)
            (output/'results.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
            if not row['passed']:
                raise SystemExit(f'Failed {label}: see {destination}')
        run(name, command)
        if name == 'first-launch':
            # Same executable directory and persisted marker, new process.
            run('first-launch-restart', [command[0], 'startup-menu', str(candidate)])
    print(f'COMPLETE cases={len(rows)} assertions={sum(row["assertions"] for row in rows)}')


if __name__ == '__main__':
    main()
