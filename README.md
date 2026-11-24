Tactrifree is a replacement driver setup for the Openport 2.0 that provides 32/64 bit J2534 interfaces and enables ECUFlash to function through a custom dll. This was tested on Windows 10/11 and no security settings have to be disabled. 

For Windows 7/8 use https://github.com/ciper/tactrifree-78

Details 
- TactriFree doesn't actually install a "driver" of its own. The cable runs on WinUSB, the USB driver built into Windows and signed by Microsoft. What gets signed is TactriFree's small setup file that points the cable at WinUSB, and it's signed on the user's own PC during install. Setup creates a certificate for that one PC, signs with it, then deletes the signing key so the certificate can never sign anything else. The J2534 DLLs and the EcuFlash DLL themselves aren't signed. 
- EcuFlash has its own built-in cable code and doesn't use J2534, so TactriFree installs a small bridge DLL (glu32.dll) in the EcuFlash folder that connects EcuFlash to the cable through WinUSB. It also blocks firmware updates, so the cable can't be bricked from EcuFlash.


A copy of ecuflash_1444870_win.exe can be found here https://drive.google.com/file/d/11rm2t4Nmlz0wD0WsUBhuyRLvZcSukvC8/view Sha-256 hash E9242D8882530FC320164F13E4107CEFF9C862F5BD2E66DEBDBEBE4895FFFA0B
