# Status

Target client: Roblox 2.744.1081, ARM64.

| Area | Status |
| --- | --- |
| Native release build | Builds successfully with the 2.744 flags. |
| Standalone APK packaging | Local packaging works with matching client inputs. |
| Editor in a joined game | Needs investigation. The last MuMu run failed during editor startup. |
| Physical ARM64 phone | Not tested for this source revision. |
| 2.744 live compatibility suite | Not complete. |

## Next steps

1. Capture the editor bootstrap error in a development build.
2. Fix startup, then test joining a second game in the same process.
3. Run the service, HTTP, and bypass checks against the 2.744 build.
4. Test a signed package on a physical ARM64 phone.

Older sUNC results belong to older client builds and do not describe this
revision.
