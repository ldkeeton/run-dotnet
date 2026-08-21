#include "util/https_download.h"
#include "util/extract_tar_gz.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// -------------------------------------------------------------------
// Logging
// -------------------------------------------------------------------
static void log(const std::string &msg) {
    std::cerr << msg << std::endl;
    std::cerr.flush();
}

// -------------------------------------------------------------------
// Fork/exec wrapper
// -------------------------------------------------------------------
static bool run_process(const fs::path &exe, char *const argv[], const std::string &label) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(exe.c_str(), argv);
        perror("[child] execv failed");
        _exit(127);
    }
    if (pid < 0) {
        perror("fork failed");
        return false;
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid failed");
        return false;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        log(label + " exited with " + std::to_string(code));
        return code == 0;
    }
    if (WIFSIGNALED(status)) {
        log(label + " killed by signal " + std::to_string(WTERMSIG(status)));
    }
    return false;
}

// -------------------------------------------------------------------
// Split https://host/path into host+path
// -------------------------------------------------------------------
static void split_url(const std::string &fullUrl, std::string &host, std::string &path) {
    if (fullUrl.find("https://") == 0) {
        auto noScheme = fullUrl.substr(8);
        auto slashPos = noScheme.find('/');
        host = noScheme.substr(0, slashPos);
        path = noScheme.substr(slashPos);
    } else {
        throw std::runtime_error("Unsupported URL: " + fullUrl);
    }
}

// -------------------------------------------------------------------
// Pick channel URL (prefer LTS + active, fallback STS)
// -------------------------------------------------------------------
static std::string pick_channel_url(const json &index, int pinnedMajor = -1) {
    std::string result;
    int bestMajor = -1;

    for (auto &entry : index["releases-index"]) {
        std::string type = entry.value("release-type", "");
        std::string phase = entry.value("support-phase", "");
        std::string ver = entry.value("channel-version", "");
        std::string url = entry.value("releases.json", "");
        if (url.empty())
            continue;

        try {
            int major = std::stoi(ver.substr(0, ver.find('.')));
            if (pinnedMajor != -1 && major != pinnedMajor)
                continue;

            if (type == "lts" && phase == "active") {
                if (major > bestMajor) {
                    bestMajor = major;
                    result = url;
                }
            }
        } catch (...) {
        }
    }

    if (!result.empty()) {
        return result;
    }

    log("No active LTS found, trying STS...");
    for (auto &entry : index["releases-index"]) {
        std::string type = entry.value("release-type", "");
        std::string ver = entry.value("channel-version", "");
        std::string url = entry.value("releases.json", "");
        if (type == "sts" && !url.empty()) {
            log("Fallback to STS " + ver);
            return url;
        }
    }

    return {};
}

// -------------------------------------------------------------------
// Find the releases.json URL for a given major channel (e.g. 8 -> 8.0)
// -------------------------------------------------------------------
static std::string find_channel_url_by_major(const json &index, int major) {
    for (auto &entry : index["releases-index"]) {
        std::string chanVer = entry.value("channel-version", "");
        std::string url = entry.value("releases.json", "");
        if (url.empty())
            continue;
        try {
            if (std::stoi(chanVer.substr(0, chanVer.find('.'))) == major)
                return url;
        } catch (...) {
        }
    }
    return {};
}

// -------------------------------------------------------------------
// Pick Hosting Bundle runtime asset (default) or SDK
// -------------------------------------------------------------------
static std::string pick_asset_url(const json &channel,
                                  const std::string &targetVersion,
                                  const std::string &rid = "linux-x64") {
    auto pickFile = [&](const json &files, const std::string &label) -> std::string {
        for (auto &f : files) {
            std::string fRid = f.value("rid", "");
            std::string fUrl = f.value("url", "");
            std::string fName = f.value("name", "");
            std::string fType = f.value("file-type", "");
            if (fRid == rid && !fUrl.empty()) {
                if (fType == "installer" || fName.find(".tar.gz") != std::string::npos) {
                    log("Selected " + label + " asset: " + fName);
                    return fUrl;
                }
            }
        }
        return {};
    };

    for (auto &release : channel["releases"]) {
        std::string ver = release.value("release-version", "");
        if (ver != targetVersion)
            continue;


        if (release.contains("sdk") && release["sdk"].contains("files")) {
            std::string url = pickFile(release["sdk"]["files"], "SDK");
            if (!url.empty())
                return url;
        }

        if (release.contains("runtime") && release["runtime"].contains("files")) {
            std::string url = pickFile(release["runtime"]["files"], "runtime");
            if (!url.empty())
                return url;
        }
    }

    log("pick_asset_url: No matching asset found for version " + targetVersion);
    return {};
}

// -------------------------------------------------------------------
// TFM detection: figure out which .NET major the project targets so we
// install an SDK whose bundled runtime can actually run the app.
// Signals, in order: *.runtimeconfig.json (the exact runtime an app was
// built for), then *.csproj <TargetFramework(s)>.
// -------------------------------------------------------------------
static int runtimeconfig_major(const fs::path &rcFile) {
    try {
        std::ifstream fin(rcFile);
        json rc;
        fin >> rc;
        const auto &ro = rc.at("runtimeOptions");

        std::string version;
        if (ro.contains("framework") && ro["framework"].contains("version")) {
            version = ro["framework"]["version"].get<std::string>();
        } else if (ro.contains("frameworks") && ro["frameworks"].is_array()) {
            // ASP.NET Core apps list frameworks as an array; prefer the base one.
            for (auto &fw : ro["frameworks"]) {
                std::string name = fw.value("name", "");
                if (name == "Microsoft.NETCore.App") {
                    version = fw.value("version", "");
                    break;
                }
            }
            if (version.empty()) {
                for (auto &fw : ro["frameworks"]) {
                    std::string v = fw.value("version", "");
                    if (!v.empty()) {
                        version = v;
                        break;
                    }
                }
            }
        }

        if (version.find('.') != std::string::npos) {
            int major = std::stoi(version.substr(0, version.find('.')));
            if (major > 0)
                return major;
        }
    } catch (...) {
    }
    return -1;
}

static int csproj_target_major(const fs::path &csproj) {
    try {
        std::ifstream fin(csproj);
        std::string content((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());

        int best = -1;
        size_t pos = 0;
        while ((pos = content.find("<TargetFramework", pos)) != std::string::npos) {
            size_t valStart = content.find('>', pos);
            if (valStart == std::string::npos)
                break;
            size_t valEnd = content.find("</", valStart);
            if (valEnd == std::string::npos)
                break;
            std::string value = content.substr(valStart + 1, valEnd - valStart - 1);

            // value looks like "net8.0" or "net8.0;net10.0" (multi-target)
            size_t tfmPos = 0;
            while ((tfmPos = value.find("net", tfmPos)) != std::string::npos) {
                size_t j = tfmPos + 3;
                if (j < value.size() && isdigit((unsigned char)value[j])) {
                    std::string num;
                    while (j < value.size() && isdigit((unsigned char)value[j]))
                        num += value[j++];
                    int major = std::atoi(num.c_str());
                    if (major > best)
                        best = major;
                }
                tfmPos += 3;
            }
            pos = valEnd + 2;
        }
        return best;
    } catch (...) {
        return -1;
    }
}

static int detect_target_major(const fs::path &dir) {
    try {
        // 1) runtimeconfig.json — what the app actually needs (run case)
        for (auto &entry : fs::directory_iterator(dir)) {
            std::string name = entry.path().filename().string();
            if (entry.path().extension() == ".json" && name.find("runtimeconfig") != std::string::npos) {
                int major = runtimeconfig_major(entry.path());
                if (major != -1) {
                    log("Detected target major " + std::to_string(major) + " from " + name);
                    return major;
                }
            }
        }
        // 2) csproj TargetFramework(s) (build case)
        for (auto &entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".csproj") {
                int major = csproj_target_major(entry.path());
                if (major != -1) {
                    log("Detected target major " + std::to_string(major) + " from " + entry.path().filename().string());
                    return major;
                }
            }
        }
    } catch (...) {
    }
    return -1;
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------
int main(int argc, char *argv[]) {
    try {
        log("dotnet bootstrapper started");

        fs::path projectRoot = fs::current_path();
        fs::path dotnetDir = projectRoot / ".dotnet";
        fs::create_directories(dotnetDir);
        fs::path versionFile = dotnetDir / "version.txt";

        fs::path storeDir = fs::path(getenv("HOME")) / ".local/share/run-dotnet";
        fs::path archivesDir = storeDir / "archives";
        fs::path versionsDir = storeDir / "versions";
        fs::create_directories(archivesDir);
        fs::create_directories(versionsDir);

        std::string pinnedVersion;
        int dotnetArgStart = 1;

        if (argc > 1) {
            std::string arg1 = argv[1];
            if (isdigit(arg1[0])) {
                pinnedVersion = arg1;
                dotnetArgStart = 2;
                log("Pinned version: " + pinnedVersion);
            }
        }

        std::string idxStr = https_get_string(
            "dotnetcli.blob.core.windows.net",
            "/dotnet/release-metadata/releases-index.json");
        json index = json::parse(idxStr);

        std::string targetVersion;
        json channelJson;

        if (!pinnedVersion.empty()) {
            std::string majorStr = pinnedVersion.substr(0, pinnedVersion.find('.'));
            int pinnedMajor = std::stoi(majorStr);

            std::string pinnedChannelUrl = find_channel_url_by_major(index, pinnedMajor);

            if (pinnedChannelUrl.empty()) {
                log("No channel found for major " + majorStr);
                return 1;
            }

            std::string host, path;
            split_url(pinnedChannelUrl, host, path);
            std::string channelStr = https_get_string(host, path);
            channelJson = json::parse(channelStr);

            if (pinnedVersion.find('.') == std::string::npos) {
                pinnedVersion = channelJson.value("latest-release", "");
                log("Resolved major " + majorStr + " to latest " + pinnedVersion);
            } else if (std::count(pinnedVersion.begin(), pinnedVersion.end(), '.') == 1) {
                std::string best;
                for (auto &release : channelJson["releases"]) {
                    std::string relVer = release.value("release-version", "");
                    if (relVer.rfind(pinnedVersion + ".", 0) == 0) {
                        if (best.empty() || relVer > best)
                            best = relVer;
                    }
                }
                if (!best.empty()) {
                    pinnedVersion = best;
                    log("Resolved " + pinnedVersion + ".* to " + best);
                }
            }
            targetVersion = pinnedVersion;

            // -- Write pinned major to version.txt (overwrite old pin)
            try {
                int pinnedMajorInt = std::stoi(pinnedVersion.substr(0, pinnedVersion.find('.')));
                std::ofstream fout(versionFile, std::ios::trunc);
                fout << pinnedMajorInt;
                log("Pinned major " + std::to_string(pinnedMajorInt) + " written into version.txt");
            } catch (...) {
                log("Warning: could not parse pinned major from " + pinnedVersion);
            }
        } else {
            int cachedMajor = -1;
            if (fs::exists(versionFile)) {
                std::ifstream fin(versionFile);
                fin >> cachedMajor;
            }

        std::string channelUrl;
        int projectMajor = detect_target_major(projectRoot);
        if (projectMajor != -1) {
            channelUrl = find_channel_url_by_major(index, projectMajor);
            if (channelUrl.empty()) {
                log("No channel found for detected major " + std::to_string(projectMajor) + ", falling back");
            }
        }
        if (channelUrl.empty() && cachedMajor != -1) {
            // ---- Cached major (from a previous run) as a fallback
            channelUrl = find_channel_url_by_major(index, cachedMajor);
        }
        if (channelUrl.empty()) {
            // ---- First-time run / nothing detected → pick active LTS
            channelUrl = pick_channel_url(index);
            if (channelUrl.empty()) {
                log("Could not determine channel URL");
                return 1;
            }
        }

            std::string host, path;
            split_url(channelUrl, host, path);
            std::string channelStr = https_get_string(host, path);
            channelJson = json::parse(channelStr);

            targetVersion = channelJson.value("latest-release", "");
            int effectiveMajor = projectMajor != -1 ? projectMajor : cachedMajor;
            if (effectiveMajor == -1) {
                // ---- First run chosen by active LTS → cache the channel major
                try {
                    int latestMajor = std::stoi(channelJson.value("channel-version", "0"));
                    std::ofstream fout(versionFile);
                    fout << latestMajor;
                    log("Pinned major " + std::to_string(latestMajor) + " into version.txt");
                } catch (...) {
                }
            } else if (effectiveMajor != cachedMajor) {
                // ---- Fresh detection / changed cache → persist it
                try {
                    std::ofstream fout(versionFile, std::ios::trunc);
                    fout << effectiveMajor;
                    log("Pinned major " + std::to_string(effectiveMajor) + " into version.txt");
                } catch (...) {
                }
            }
        }

        std::string downloadUrl = pick_asset_url(channelJson, targetVersion);
        if (downloadUrl.empty()) {
            log("No asset found for version " + targetVersion);
            return 1;
        }

        std::string host, path;
        split_url(downloadUrl, host, path);

        fs::path archivePath = archivesDir / fs::path(path).filename();
        std::string baseName = fs::path(path).stem().stem().string();
        fs::path extractDir = versionsDir / (targetVersion + "-" + baseName);
        fs::path dotnetBin = extractDir / "dotnet";
 

        if (!fs::exists(archivePath)) {
            log("Downloading " + path);
            https_download(host, path, archivePath);
        }
        if (!fs::exists(dotnetBin)) {
            fs::create_directories(extractDir);
            if (!extract_tar_gz(archivePath.string(), extractDir.string())) {
                log("Extraction failed");
                return 1;
            }
        }

        for (auto &e : fs::directory_iterator(dotnetDir)) {
            if (e.path() == versionFile)
                continue;
            fs::remove_all(e.path());
        }
        for (auto &entry : fs::directory_iterator(extractDir))
            fs::create_symlink(entry.path(), dotnetDir / entry.path().filename());

        fs::path projectDotnetBin = dotnetDir / "dotnet";
        if (!fs::exists(projectDotnetBin)) {
            log("dotnet binary not found after extraction");
            return 1;
        }

        // projectDotnetBin points to .../.dotnet/dotnet
        std::string dotnetRoot = projectDotnetBin.parent_path().string();

        // DOTNET_ROOT=<repo>/.dotnet
        setenv("DOTNET_ROOT", dotnetRoot.c_str(), 1);

        // PATH=<repo>/.dotnet:$PATH
        const char* oldPath = getenv("PATH");
        std::string newPath = dotnetRoot + ":" + (oldPath ? oldPath : "");
        setenv("PATH", newPath.c_str(), 1);

        fs::path csproj;
        for (auto &entry : fs::directory_iterator(projectRoot)) {
            if (entry.path().extension() == ".csproj") {
                csproj = entry.path();
                break;
            }
        }
        if (!csproj.empty()) {
            char *restoreArgs[] = {
                const_cast<char *>(projectDotnetBin.c_str()),
                const_cast<char *>("restore"),
                const_cast<char *>(csproj.c_str()),
                nullptr};
            if (!run_process(projectDotnetBin, restoreArgs, "dotnet restore")) {
                return 1;
            }
        }

        if (argc <= dotnetArgStart) {
            log(std::string("Usage: ") + argv[0] + "[X[.Y[.Z]]] <args to dotnet>");
            return 1;
        }

        std::vector<char *> newArgs;
        newArgs.push_back(const_cast<char *>(projectDotnetBin.c_str()));
        for (int i = dotnetArgStart; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);
        
        std::cerr << "[debug] DOTNET_ROOT=" << getenv("DOTNET_ROOT") << "\n";
        std::cerr << "[debug] PATH=" << getenv("PATH") << "\n";
        
        return run_process(projectDotnetBin, newArgs.data(), "dotnet main") ? 0 : 1;
    } catch (const std::exception &e) {
        log(std::string("Error: ") + e.what());
        return 1;
    }
}