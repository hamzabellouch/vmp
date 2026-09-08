# Security Policy

## Supported Versions

The following versions of VMP are currently supported with security, stability, and decoder safety updates.

| Version       | Supported          |
| ------------- | ------------------ |
| >= 0.0.2-beta | :white_check_mark: |
| < 0.0.2-beta  | :x:                |



## Reporting a Vulnerability

If you discover a security vulnerability, memory corruption issue, unexpected hardware decoder privilege escalation, or any issue that could negatively affect system stability or user safety, please report it responsibly.

### Before Reporting
Please make sure that:
- The issue is reproducible
- You are using the latest supported version of VMP
- The issue is not caused by corrupted third-party graphics drivers, unsupported proprietary binary blobs, or arbitrary system-level kernel patches

### How to Report
You can report vulnerabilities through:
- GitHub Issues (for non-sensitive bug reports and general questions)
- Direct private contact email (`hamzabellouchcontact@gmail.com`) for sensitive vulnerabilities or critical security concerns

When reporting, please include:
- Linux distribution, desktop environment, and kernel version (`uname -r`)
- GPU model and installed graphics driver version (e.g., Mesa VA-API, NVIDIA proprietary driver)
- VMP application version (`vmp_engine --version` or `vmp_cli --version`)
- Sample video stream specs, container format, or a minimal reproducible sample file
- Reproduction steps, terminal output, GDB backtrace, or AddressSanitizer logs if available
- A clear explanation of the potential security or system impact

### Response Policy
Security reports are reviewed as quickly as possible.  
If the issue is confirmed:
- The vulnerability will be investigated, verified, and patched
- A security fix will be included in the next update release
- Credit will be given to the reporter upon disclosure if requested

If the report is invalid, incomplete, or not reproducible, it may be closed with appropriate explanation.



## Security & Architecture Notes

VMP is engineered with a strict **Native-Performance, Offline & Memory-Safe Security Model**:

- **100% Offline & Local Execution:** VMP operates entirely offline without telemetry transmission, remote analytics, or background internet communication. Your media streams and telemetry reports remain strictly on your local machine.
- **Strict Video-Only Isolation:** Static image formats and non-video payload files are rejected immediately with exit code `1` at startup. This prevents malicious image parser exploits and protects system MIME file associations from being hijacked.
- **Root-Free Operation:** VMP is designed to run safely within unprivileged user space. Neither GUI playback nor headless CLI benchmarking requires `root` or `sudo` permissions during regular operation.
- **Safe Decoder Fallback:** Hardware decoders (VA-API / NVDEC) run with defensive buffer bounds. If a malformed or malicious video bitstream triggers driver memory allocation errors, VMP automatically falls back to the isolated multi-threaded AVX2 CPU engine without crashing the desktop compositor.
- **Transient Memory & Texture Management:** Decoded ring buffer frames and GPU texture memory are cleared and deallocated upon playback termination or window closure, minimizing memory footprint and preventing dangling pointers or information leaks.
