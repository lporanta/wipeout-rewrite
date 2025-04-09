Here is all tracks result
Folder 	Name 		Comment
TRACK01 	Talon's Reach 	freezes during loading (1)
TRACK08 	Sagarmatha 	✔️ 	
TRACK13 	Valparaiso 	freezes during loading (1)
TRACK20 	Phenitia Park 	✔️ 	
TRACK02 	Gare d'Europa 	✔️ 	missing main track texture (2)
TRACK17 	Odessa Keys 	✔️ 	
TRACK06 	Vostok Island 	✔️ 	
TRACK07 	Spilskinanke 	missing main track texture (2)
crashes at the end of the pit lane junction (3)

1. it loop indefinitly because drone initialization code is looking for a track jump and those tracks don't contain any. If related code is commented out, it works perfectly. A better fix is to stop looping on sections if no jump is found.

2. the main track texture is missing because texture tile indexes are sometimes stored in TRACK.TEX file. See here.

2. might be related to track sections and views (TRS/TRV). Does not crash if pitlane is used. Phoboslab version does not seems to be affected by that issue.

If you want to try it yourself

Prerequisites :

* Wipeout XL / 2097 iso
* a working copy of Wipeout Phantom Edition with Wipeout data already extracted into wipeoutgame folder. It's advised to create a backup of it (to not mess with your timings and such)

1. Extract all files from Wipeout XL / 2097 iso into a new folder. You can use PowerISO or any tool that open CUE/BIN files.
2. Copy tracks files from one Wipeout XL / 2097 folder to another Wipeout folder.

Eg: copy files from WipeoutXL/TRACK08 into wipeoutgame/WIPEOUT/TRACK06 to play Sagarmatha using Terramax Venom.

You can use this table (and the one above) as a reference :
Venom 	Rapier 	Name
TRACK02 	TRACK03 	Altima VII
TRACK04 	TRACK05 	Karbonis V
TRACK01 	TRACK06 	Terramax
TRACK12 	TRACK07 	Korodera
TRACK08 	TRACK11 	Arridos IV
TRACK09 	TRACK13 	Silverstream
TRACK10 	TRACK14 	Firestar

3. ships (optional) : copy COMMON/TERRY.CMP and COMMON/TERRY.PRM into wipeoutgame/COMMON folder. Then run those commands inside wipeoutgame/COMMON : copy /b /y TERRY.PRM + TERRY.PRM ALLSH.PRM copy /y TERRY.CMP ALLSH.CMP.

Fixing issues :

* Freeze during load (Talon's Reach / Valparaiso) : open TRACK.TRS file with a hexadecimal editor (such as HxD) and mark the first track section as jump by changing the byte at offset 151 from 00 to 01.
* Missing textures (Gare d'Europa / Spilskinanke) : use the following python script to copy data from TEX file into TRV. Copy the python script into the TRACKxx folder, then run this command inside that folder : python wipeout_tex_import.py. You might need to install Python before.
* Crash near junction (Spilskinanke) : no fix, use left pitlane and avoid the right lane.
