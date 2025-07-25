#include "LSPClangdDetector.hpp"

#include "AsyncProcess/asyncprocess.h"
#include "CompilerLocator/CompilerLocatorCLANG.h"
#include "Platform/MSYS2.hpp"
#include "Platform/Platform.hpp"
#include "clFilesCollector.h"
#include "file_logger.h"
#include "globals.h"
#include "procutils.h"
#include "tags_options_data.h"

#include <wx/filename.h>
#include <wx/regex.h>

LSPClangdDetector::LSPClangdDetector()
    : LSPDetector("clangd")
{
}

LSPClangdDetector::~LSPClangdDetector() {}

bool LSPClangdDetector::DoLocate()
{
    const auto path = ThePlatform->WhichWithVersion("clangd", { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 });
    if (!path) {
        return false;
    }

    LSP_DEBUG() << "Found clangd ==>" << *path << endl;
    ConfigureFile(*path);
    return true;
}

void LSPClangdDetector::ConfigureFile(const wxFileName& clangdExe)
{
    LSP_DEBUG() << "==> Found" << clangdExe;
    wxString command;
    command << clangdExe.GetFullPath();
    ::WrapWithQuotes(command);

    clConfig ccConfig("code-completion.conf");
    TagsOptionsData tagsOptionsData;
    ccConfig.ReadItem(&tagsOptionsData);

    size_t limit_results = tagsOptionsData.GetCcNumberOfDisplayItems();
    command << " --limit-results=" << limit_results
            << " --header-insertion-decorators=0 --compile-commands-dir=$(WorkspacePath) --background-index";

    SetCommand(command);
    // Add support for the languages
    GetLanguages().Add("c");
    GetLanguages().Add("cpp");
    SetConnectionString("stdio");
}
