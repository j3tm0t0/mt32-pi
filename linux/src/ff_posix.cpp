//
// ff_posix.cpp - FatFs API subset implemented on POSIX for mt32-pi on Linux
//

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>

#include <cstring>
#include <map>
#include <string>

// Keep a name for the POSIX directory stream type before ff.h renames DIR
typedef DIR HostDIR;

#include <fatfs/ff.h>

namespace
{
	std::map<std::string, std::string>& VolumeRoots()
	{
		static std::map<std::string, std::string> Roots{{"SD", "."}};
		return Roots;
	}
}

void ff_set_volume_root(const char* pVolume, const char* pHostPath)
{
	if (pHostPath && *pHostPath)
		VolumeRoots()[pVolume] = pHostPath;
	else
		VolumeRoots().erase(pVolume);
}

std::string ff_translate_path(const char* pPath)
{
	std::string Volume = "SD";
	const char* pRest  = pPath;

	if (const char* pColon = strchr(pPath, ':'))
	{
		Volume.assign(pPath, pColon - pPath);
		pRest = pColon + 1;
	}

	const auto It = VolumeRoots().find(Volume);
	if (It == VolumeRoots().end())
		return std::string();

	while (*pRest == '/')
		++pRest;

	std::string Result = It->second;
	if (*pRest)
	{
		if (Result.empty() || Result.back() != '/')
			Result += '/';
		Result += pRest;
	}

	return Result;
}

FRESULT f_open(FIL* fp, const char* path, BYTE mode)
{
	const std::string HostPath = ff_translate_path(path);
	if (HostPath.empty())
		return FR_INVALID_DRIVE;

	if (mode & ~FA_READ)
		return FR_DENIED;

	struct stat Stat;
	if (stat(HostPath.c_str(), &Stat) != 0 || !S_ISREG(Stat.st_mode))
		return FR_NO_FILE;

	fp->pFile = fopen(HostPath.c_str(), "rb");
	if (!fp->pFile)
		return FR_NO_FILE;

	fp->nSize = static_cast<FSIZE_t>(Stat.st_size);
	return FR_OK;
}

FRESULT f_close(FIL* fp)
{
	if (!fp->pFile)
		return FR_INVALID_OBJECT;

	fclose(fp->pFile);
	fp->pFile = nullptr;
	return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
	if (!fp->pFile)
		return FR_INVALID_OBJECT;

	const size_t nRead = fread(buff, 1, btr, fp->pFile);
	*br                = static_cast<UINT>(nRead);
	return (nRead < btr && ferror(fp->pFile)) ? FR_DISK_ERR : FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
{
	if (!fp->pFile)
		return FR_INVALID_OBJECT;

	return fseeko(fp->pFile, static_cast<off_t>(ofs), SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}

FSIZE_t f_tell(FIL* fp)
{
	if (!fp->pFile)
		return 0;

	return static_cast<FSIZE_t>(ftello(fp->pFile));
}

FRESULT f_findnext(DIR* dp, FILINFO* fno)
{
	if (dp->nNext < dp->Entries.size())
		*fno = dp->Entries[dp->nNext++];
	else
		fno->fname[0] = '\0';

	return FR_OK;
}

FRESULT f_findfirst(DIR* dp, FILINFO* fno, const char* path, const char* pattern)
{
	dp->Entries.clear();
	dp->nNext     = 0;
	fno->fname[0] = '\0';

	const std::string HostPath = ff_translate_path(path);
	if (HostPath.empty())
		return FR_INVALID_DRIVE;

	HostDIR* pHostDir = opendir(HostPath.c_str());
	if (!pHostDir)
		return FR_NO_PATH;

	const char* pPattern = pattern ? pattern : "*";
	while (dirent* pEntry = readdir(pHostDir))
	{
		if (!strcmp(pEntry->d_name, ".") || !strcmp(pEntry->d_name, ".."))
			continue;

		if (fnmatch(pPattern, pEntry->d_name, 0) != 0)
			continue;

		FILINFO Info{};
		strncpy(Info.fname, pEntry->d_name, sizeof(Info.fname) - 1);

		if (pEntry->d_name[0] == '.')
			Info.fattrib |= AM_HID;

		// Follow symlinks so that linked ROMs/SoundFonts are picked up
		struct stat Stat;
		if (fstatat(dirfd(pHostDir), pEntry->d_name, &Stat, 0) == 0)
		{
			if (S_ISDIR(Stat.st_mode))
				Info.fattrib |= AM_DIR;
			else if (!S_ISREG(Stat.st_mode))
				Info.fattrib |= AM_SYS;
			Info.fsize = static_cast<FSIZE_t>(Stat.st_size);
		}
		else
			Info.fattrib |= AM_SYS;

		dp->Entries.push_back(Info);
	}

	closedir(pHostDir);
	return f_findnext(dp, fno);
}

FRESULT f_closedir(DIR* dp)
{
	dp->Entries.clear();
	dp->nNext = 0;
	return FR_OK;
}
