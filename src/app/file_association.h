// Double-clicking a .wax file in the file manager opens it in Wax.
//
// Windows only. The association is written under HKEY_CURRENT_USER\Software\Classes, so it needs no administrator rights and
// only affects the current user. On other systems the file manager's own "open with" is the way, and these functions say so.
#pragma once

#include <string>
#include <vector>

namespace wax::app {

// True where Wax can register itself as the opener of .wax files (Windows).
bool fileAssociationSupported();

// True when .wax files already open with the Wax that is running now.
bool isOpenerOfWaxFiles();

// Makes .wax files open with the running Wax. Returns "" on success, else a sentence saying what went wrong.
std::string registerAsOpenerOfWaxFiles();

// The command line as UTF-8, one entry per argument with the program name first. Windows hands a double-clicked file's path to main()
// in the ANSI code page, which loses any letter outside it (an accent, say), so the wide command line is read and converted.
// Empty where main()'s own argv is already UTF-8.
std::vector<std::string> commandLineUtf8();

}  // namespace wax::app
