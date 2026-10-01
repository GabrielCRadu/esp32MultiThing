Set WshShell = CreateObject("WScript.Shell")
WshShell.Run "pythonw.exe """ & WScript.ScriptFullName & """\..\music.py", 0, False
