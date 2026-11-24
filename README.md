TactriFree is a replacement driver setup for the OpenPort 2.0 that provides 32/64-bit J2534 interfaces and enables EcuFlash to work through a custom DLL. Tested on Windows 10/11 with Secure Boot, driver signature enforcement and Memory integrity (HVCI) all enabled.

For Windows 7, 8 use https://github.com/ciper/tactrifree-78

Details:
- TactriFree doesn't actually install a "driver" of its own. The cable runs on WinUSB, the USB driver built into Windows and signed by Microsoft. What gets signed is TactriFree's small setup file that points the cable at WinUSB, and it's signed on the user's own PC during install. Setup creates a certificate for that one PC, signs with it, then deletes the signing key so the certificate can never sign anything else. The J2534 DLLs and the EcuFlash DLL themselves aren't signed.
- EcuFlash has its own built-in cable code and doesn't use J2534, so TactriFree installs a small bridge DLL (glu32.dll) in the EcuFlash folder that connects EcuFlash to the cable through WinUSB. It also blocks firmware updates, so the cable can't be bricked from EcuFlash.

EcuFlash 1.44.4870 (the version TactriFree was tested with): https://drive.google.com/file/d/11rm2t4Nmlz0wD0WsUBhuyRLvZcSukvC8/view
SHA-256: E9242D8882530FC320164F13E4107CEFF9C862F5BD2E66DEBDBEBE4895FFFA0B
When installing EcuFlash, decline its driver install prompts (the installer also looks for the original drivers and removes them)
