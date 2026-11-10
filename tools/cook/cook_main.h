#pragma once

// The cooker's command line (glTF -> .fpak): falcon.exe runs it as `falcon cook <args>`. argv[0] is
// the subcommand's name; returns the process exit code.
int cookMain(int argc, char** argv);
