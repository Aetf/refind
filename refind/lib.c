/*
 * refind/lib.c
 * General library functions
 *
 * Copyright (c) 2006-2009 Christoph Pfisterer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *  * Neither the name of Christoph Pfisterer nor the names of the
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Modifications copyright (c) 2012-2015 Roderick W. Smith
 *
 * Modifications distributed under the terms of the GNU General Public
 * License (GPL) version 3 (GPLv3), a copy of which must be distributed
 * with this source code or binaries made from it.
 *
 */

#include "global.h"
#include "lib.h"
#include "icns.h"
#include "screen.h"
#include "../include/refit_call_wrapper.h"
#include "../include/RemovableMedia.h"
#include "gpt.h"
#include "config.h"
#include "../EfiLib/LegacyBios.h"

#ifdef __MAKEWITH_GNUEFI
#define EfiReallocatePool ReallocatePool
#else
#define LibLocateHandle gBS->LocateHandleBuffer
#define DevicePathProtocol gEfiDevicePathProtocolGuid
#define BlockIoProtocol gEfiBlockIoProtocolGuid
#define LibFileSystemInfo EfiLibFileSystemInfo
#define LibOpenRoot EfiLibOpenRoot
EFI_DEVICE_PATH EndDevicePath[] = {
   {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {END_DEVICE_PATH_LENGTH, 0}}
};

//#define EndDevicePath DevicePath
#endif

// "Magic" signatures for various filesystems
#define FAT_MAGIC                        0xAA55
#define EXT2_SUPER_MAGIC                 0xEF53
#define HFSPLUS_MAGIC1                   0x2B48
#define HFSPLUS_MAGIC2                   0x5848
#define REISERFS_SUPER_MAGIC_STRING      "ReIsErFs"
#define REISER2FS_SUPER_MAGIC_STRING     "ReIsEr2Fs"
#define REISER2FS_JR_SUPER_MAGIC_STRING  "ReIsEr3Fs"
#define BTRFS_SIGNATURE                  "_BHRfS_M"
#define XFS_SIGNATURE                    "XFSB"
#define NTFS_SIGNATURE                   "NTFS    "

// variables

EFI_HANDLE       SelfImageHandle;
EFI_LOADED_IMAGE *SelfLoadedImage;
EFI_FILE         *SelfRootDir;
EFI_FILE         *SelfDir;
CHAR16           *SelfDirPath;

REFIT_VOLUME     *SelfVolume = NULL;
REFIT_VOLUME     **Volumes = NULL;
UINTN            VolumesCount = 0;
extern GPT_DATA *gPartitions;

// Maximum size for disk sectors
#define SECTOR_SIZE 4096

// Number of bytes to read from a partition to determine its filesystem type
// and identify its boot loader, and hence probable BIOS-mode OS installation
#define SAMPLE_SIZE 69632 /* 68 KiB -- ReiserFS superblock begins at 64 KiB */


// functions

static EFI_STATUS FinishInitRefitLib(VOID);

static VOID UninitVolumes(VOID);

//
// self recognition stuff
//

// Converts forward slashes to backslashes, removes duplicate slashes, and
// removes slashes from both the start and end of the pathname.
// Necessary because some (buggy?) EFI implementations produce "\/" strings
// in pathnames, because some user inputs can produce duplicate directory
// separators, and because we want consistent start and end slashes for
// directory comparisons. A special case: If the PathName refers to root,
// return "/", since some firmware implementations flake out if this
// isn't present.
VOID CleanUpPathNameSlashes(IN OUT CHAR16 *PathName) {
   CHAR16   *NewName;
   UINTN    i, Length, FinalChar = 0;
   BOOLEAN  LastWasSlash = FALSE;

   Length = StrLen(PathName);
   NewName = AllocateZeroPool(sizeof(CHAR16) * (Length + 2));
   if (NewName != NULL) {
      for (i = 0; i < StrLen(PathName); i++) {
         if ((PathName[i] == L'/') || (PathName[i] == L'\\')) {
            if ((!LastWasSlash) && (FinalChar != 0))
               NewName[FinalChar++] = L'\\';
            LastWasSlash = TRUE;
         } else {
            NewName[FinalChar++] = PathName[i];
            LastWasSlash = FALSE;
         } // if/else
      } // for
      NewName[FinalChar] = 0;
      if ((FinalChar > 0) && (NewName[FinalChar - 1] == L'\\'))
         NewName[--FinalChar] = 0;
      if (FinalChar == 0) {
         NewName[0] = L'\\';
         NewName[1] = 0;
      }
      // Copy the transformed name back....
      StrCpy(PathName, NewName);
      FreePool(NewName);
   } // if allocation OK
} // CleanUpPathNameSlashes()

// Splits an EFI device path into device and filename components. For instance, if InString is
// PciRoot(0x0)/Pci(0x1f,0x2)/Ata(Secondary,Master,0x0)/HD(2,GPT,8314ae90-ada3-48e9-9c3b-09a88f80d921,0x96028,0xfa000)/\bzImage-3.5.1.efi,
// this function will truncate that input to
// PciRoot(0x0)/Pci(0x1f,0x2)/Ata(Secondary,Master,0x0)/HD(2,GPT,8314ae90-ada3-48e9-9c3b-09a88f80d921,0x96028,0xfa000)
// and return bzImage-3.5.1.efi as its return value.
// It does this by searching for the last ")" character in InString, copying everything
// after that string (after some cleanup) as the return value, and truncating the original
// input value.
// If InString contains no ")" character, this function leaves the original input string
// unmodified and also returns that string. If InString is NULL, this function returns NULL.
static CHAR16* SplitDeviceString(IN OUT CHAR16 *InString) {
   INTN i;
   CHAR16 *FileName = NULL;
   BOOLEAN Found = FALSE;

   if (InString != NULL) {
      i = StrLen(InString) - 1;
      while ((i >= 0) && (!Found)) {
         if (InString[i] == L')') {
            Found = TRUE;
            FileName = StrDuplicate(&InString[i + 1]);
            CleanUpPathNameSlashes(FileName);
            InString[i + 1] = '\0';
         } // if
         i--;
      } // while
      if (FileName == NULL)
         FileName = StrDuplicate(InString);
   } // if
   return FileName;
} // static CHAR16* SplitDeviceString()

EFI_STATUS InitRefitLib(IN EFI_HANDLE ImageHandle)
{
    EFI_STATUS  Status;
    CHAR16      *DevicePathAsString, *Temp;

    SelfImageHandle = ImageHandle;
    Status = refit_call3_wrapper(BS->HandleProtocol, SelfImageHandle, &LoadedImageProtocol, (VOID **) &SelfLoadedImage);
    if (CheckFatalError(Status, L"while getting a LoadedImageProtocol handle"))
        return EFI_LOAD_ERROR;

    // find the current directory
    DevicePathAsString = DevicePathToStr(SelfLoadedImage->FilePath);
    CleanUpPathNameSlashes(DevicePathAsString);
    MyFreePool(SelfDirPath);
    Temp = FindPath(DevicePathAsString);
    SelfDirPath = SplitDeviceString(Temp);
    MyFreePool(DevicePathAsString);
    MyFreePool(Temp);

    return FinishInitRefitLib();
}

// called before running external programs to close open file handles
VOID UninitRefitLib(VOID)
{
    // This piece of code was made to correspond to weirdness in ReinitRefitLib().
    // See the comment on it there.
    if(SelfRootDir == SelfVolume->RootDir)
        SelfRootDir=0;

    UninitVolumes();

    if (SelfDir != NULL) {
        refit_call1_wrapper(SelfDir->Close, SelfDir);
        SelfDir = NULL;
    }

    if (SelfRootDir != NULL) {
       refit_call1_wrapper(SelfRootDir->Close, SelfRootDir);
       SelfRootDir = NULL;
    }
}

// called after running external programs to re-open file handles
EFI_STATUS ReinitRefitLib(VOID)
{
    ReinitVolumes();

    if ((ST->Hdr.Revision >> 16) == 1) {
       // Below two lines were in rEFIt, but seem to cause system crashes or
       // reboots when launching OSes after returning from programs on most
       // systems. OTOH, my Mac Mini produces errors about "(re)opening our
       // installation volume" (see the next function) when returning from
       // programs when these two lines are removed, and it often crashes
       // when returning from a program or when launching a second program
       // with these lines removed. Therefore, the preceding if() statement
       // executes these lines only on EFIs with a major version number of 1
       // (which Macs have) and not with 2 (which UEFI PCs have). My selection
       // of hardware on which to test is limited, though, so this may be the
       // wrong test, or there may be a better way to fix this problem.
       // TODO: Figure out cause of above weirdness and fix it more
       // reliably!
       if (SelfVolume != NULL && SelfVolume->RootDir != NULL)
          SelfRootDir = SelfVolume->RootDir;
    } // if

    return FinishInitRefitLib();
}

static EFI_STATUS FinishInitRefitLib(VOID)
{
    EFI_STATUS  Status;

    if (SelfRootDir == NULL) {
        SelfRootDir = LibOpenRoot(SelfLoadedImage->DeviceHandle);
        if (SelfRootDir == NULL) {
            CheckError(EFI_LOAD_ERROR, L"while (re)opening our installation volume");
            return EFI_LOAD_ERROR;
        }
    }

    Status = refit_call5_wrapper(SelfRootDir->Open, SelfRootDir, &SelfDir, SelfDirPath, EFI_FILE_MODE_READ, 0);
    if (CheckFatalError(Status, L"while opening our installation directory"))
        return EFI_LOAD_ERROR;

    return EFI_SUCCESS;
}

//
// EFI variable read and write functions
//

// From gummiboot: Retrieve a raw EFI variable.
// Returns EFI status
EFI_STATUS EfivarGetRaw(EFI_GUID *vendor, CHAR16 *name, CHAR8 **buffer, UINTN *size) {
   CHAR8 *buf;
   UINTN l;
   EFI_STATUS err;

   l = sizeof(CHAR16 *) * EFI_MAXIMUM_VARIABLE_SIZE;
   buf = AllocatePool(l);
   if (!buf)
      return EFI_OUT_OF_RESOURCES;

   err = refit_call5_wrapper(RT->GetVariable, name, vendor, NULL, &l, buf);
   if (EFI_ERROR(err) == EFI_SUCCESS) {
      *buffer = buf;
      if (size)
         *size = l;
   } else
      MyFreePool(buf);
   return err;
} // EFI_STATUS EfivarGetRaw()

// From gummiboot: Set an EFI variable
EFI_STATUS EfivarSetRaw(EFI_GUID *vendor, CHAR16 *name, CHAR8 *buf, UINTN size, BOOLEAN persistent) {
   UINT32 flags;

   flags = EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS;
   if (persistent)
      flags |= EFI_VARIABLE_NON_VOLATILE;

   return refit_call5_wrapper(RT->SetVariable, name, vendor, flags, size, buf);
} // EFI_STATUS EfivarSetRaw()

//
// list functions
//

VOID AddListElement(IN OUT VOID ***ListPtr, IN OUT UINTN *ElementCount, IN VOID *NewElement)
{
    UINTN AllocateCount;

    if ((*ElementCount & 15) == 0) {
        AllocateCount = *ElementCount + 16;
        if (*ElementCount == 0)
            *ListPtr = AllocatePool(sizeof(VOID *) * AllocateCount);
        else
            *ListPtr = EfiReallocatePool(*ListPtr, sizeof(VOID *) * (*ElementCount), sizeof(VOID *) * AllocateCount);
    }
    (*ListPtr)[*ElementCount] = NewElement;
    (*ElementCount)++;
} /* VOID AddListElement() */

VOID FreeList(IN OUT VOID ***ListPtr, IN OUT UINTN *ElementCount)
{
    UINTN i;

    if ((*ElementCount > 0) && (**ListPtr != NULL)) {
        for (i = 0; i < *ElementCount; i++) {
            // TODO: call a user-provided routine for each element here
            MyFreePool((*ListPtr)[i]);
        }
        MyFreePool(*ListPtr);
    }
} // VOID FreeList()

//
// firmware device path discovery
//

static UINT8 LegacyLoaderMediaPathData[] = {
    0x04, 0x06, 0x14, 0x00, 0xEB, 0x85, 0x05, 0x2B,
    0xB8, 0xD8, 0xA9, 0x49, 0x8B, 0x8C, 0xE2, 0x1B,
    0x01, 0xAE, 0xF2, 0xB7, 0x7F, 0xFF, 0x04, 0x00,
};
static EFI_DEVICE_PATH *LegacyLoaderMediaPath = (EFI_DEVICE_PATH *)LegacyLoaderMediaPathData;

VOID ExtractLegacyLoaderPaths(EFI_DEVICE_PATH **PathList, UINTN MaxPaths, EFI_DEVICE_PATH **HardcodedPathList)
{
    EFI_STATUS          Status;
    UINTN               HandleCount = 0;
    UINTN               HandleIndex, HardcodedIndex;
    EFI_HANDLE          *Handles;
    EFI_HANDLE          Handle;
    UINTN               PathCount = 0;
    UINTN               PathIndex;
    EFI_LOADED_IMAGE    *LoadedImage;
    EFI_DEVICE_PATH     *DevicePath;
    BOOLEAN             Seen;

    MaxPaths--;  // leave space for the terminating NULL pointer

    // get all LoadedImage handles
    Status = LibLocateHandle(ByProtocol, &LoadedImageProtocol, NULL, &HandleCount, &Handles);
    if (CheckError(Status, L"while listing LoadedImage handles")) {
        if (HardcodedPathList) {
            for (HardcodedIndex = 0; HardcodedPathList[HardcodedIndex] && PathCount < MaxPaths; HardcodedIndex++)
                PathList[PathCount++] = HardcodedPathList[HardcodedIndex];
        }
        PathList[PathCount] = NULL;
        return;
    }
    for (HandleIndex = 0; HandleIndex < HandleCount && PathCount < MaxPaths; HandleIndex++) {
        Handle = Handles[HandleIndex];

        Status = refit_call3_wrapper(BS->HandleProtocol, Handle, &LoadedImageProtocol, (VOID **) &LoadedImage);
        if (EFI_ERROR(Status))
            continue;  // This can only happen if the firmware scewed up, ignore it.

        Status = refit_call3_wrapper(BS->HandleProtocol, LoadedImage->DeviceHandle, &DevicePathProtocol, (VOID **) &DevicePath);
        if (EFI_ERROR(Status))
            continue;  // This happens, ignore it.

        // Only grab memory range nodes
        if (DevicePathType(DevicePath) != HARDWARE_DEVICE_PATH || DevicePathSubType(DevicePath) != HW_MEMMAP_DP)
            continue;

        // Check if we have this device path in the list already
        // WARNING: This assumes the first node in the device path is unique!
        Seen = FALSE;
        for (PathIndex = 0; PathIndex < PathCount; PathIndex++) {
            if (DevicePathNodeLength(DevicePath) != DevicePathNodeLength(PathList[PathIndex]))
                continue;
            if (CompareMem(DevicePath, PathList[PathIndex], DevicePathNodeLength(DevicePath)) == 0) {
                Seen = TRUE;
                break;
            }
        }
        if (Seen)
            continue;

        PathList[PathCount++] = AppendDevicePath(DevicePath, LegacyLoaderMediaPath);
    }
    MyFreePool(Handles);

    if (HardcodedPathList) {
        for (HardcodedIndex = 0; HardcodedPathList[HardcodedIndex] && PathCount < MaxPaths; HardcodedIndex++)
            PathList[PathCount++] = HardcodedPathList[HardcodedIndex];
    }
    PathList[PathCount] = NULL;
}

//
// volume functions
//

// Return a pointer to a string containing a filesystem type name. If the
// filesystem type is unknown, a blank (but non-null) string is returned.
// The returned variable is a constant that should NOT be freed.
static CHAR16 *FSTypeName(IN UINT32 TypeCode) {
   CHAR16 *retval = NULL;

   switch (TypeCode) {
      case FS_TYPE_WHOLEDISK:
         retval = L" whole disk";
         break;
      case FS_TYPE_FAT:
         retval = L" FAT";
         break;
      case FS_TYPE_HFSPLUS:
         retval = L" HFS+";
         break;
      case FS_TYPE_EXT2:
         retval = L" ext2";
         break;
      case FS_TYPE_EXT3:
         retval = L" ext3";
         break;
      case FS_TYPE_EXT4:
         retval = L" ext4";
         break;
      case FS_TYPE_REISERFS:
         retval = L" ReiserFS";
         break;
      case FS_TYPE_BTRFS:
         retval = L" Btrfs";
         break;
      case FS_TYPE_XFS:
         retval = L" XFS";
         break;
      case FS_TYPE_ISO9660:
         retval = L" ISO-9660";
         break;
      case FS_TYPE_NTFS:
         retval = L" NTFS";
         break;
      default:
         retval = L"";
         break;
   } // switch
   return retval;
} // CHAR16 *FSTypeName()

// Identify the filesystem type and record the filesystem's UUID/serial number,
// if possible. Expects a Buffer containing the first few (normally at least
// 4096) bytes of the filesystem. Sets the filesystem type code in Volume->FSType
// and the UUID/serial number in Volume->VolUuid. Note that the UUID value is
// recognized differently for each filesystem, and is currently supported only
// for NTFS, ext2/3/4fs, and ReiserFS (and for NTFS it's really a 64-bit serial
// number not a UUID or GUID). If the UUID can't be determined, it's set to 0.
// Also, the UUID is just read directly into memory; it is *NOT* valid when
// displayed by GuidAsString() or used in other GUID/UUID-manipulating
// functions. (As I write, it's being used merely to detect partitions that are
// part of a RAID 1 array.)
static VOID SetFilesystemData(IN UINT8 *Buffer, IN UINTN BufferSize, IN OUT REFIT_VOLUME *Volume) {
   UINT32       *Ext2Incompat, *Ext2Compat;
   UINT16       *Magic16;
   char         *MagicString;
   EFI_FILE     *RootDir;

   if ((Buffer != NULL) && (Volume != NULL)) {
      SetMem(&(Volume->VolUuid), sizeof(EFI_GUID), 0);
      Volume->FSType = FS_TYPE_UNKNOWN;

      if (BufferSize >= (1024 + 100)) {
         Magic16 = (UINT16*) (Buffer + 1024 + 56);
         if (*Magic16 == EXT2_SUPER_MAGIC) { // ext2/3/4
            Ext2Compat = (UINT32*) (Buffer + 1024 + 92);
            Ext2Incompat = (UINT32*) (Buffer + 1024 + 96);
            if ((*Ext2Incompat & 0x0040) || (*Ext2Incompat & 0x0200)) { // check for extents or flex_bg
               Volume->FSType = FS_TYPE_EXT4;
            } else if (*Ext2Compat & 0x0004) { // check for journal
               Volume->FSType = FS_TYPE_EXT3;
            } else { // none of these features; presume it's ext2...
               Volume->FSType = FS_TYPE_EXT2;
            }
            CopyMem(&(Volume->VolUuid), Buffer + 1024 + 104, sizeof(EFI_GUID));
            return;
         }
      } // search for ext2/3/4 magic

      if (BufferSize >= (65536 + 100)) {
         MagicString = (char*) (Buffer + 65536 + 52);
         if ((CompareMem(MagicString, REISERFS_SUPER_MAGIC_STRING, 8) == 0) ||
             (CompareMem(MagicString, REISER2FS_SUPER_MAGIC_STRING, 9) == 0) ||
             (CompareMem(MagicString, REISER2FS_JR_SUPER_MAGIC_STRING, 9) == 0)) {
            Volume->FSType = FS_TYPE_REISERFS;
            CopyMem(&(Volume->VolUuid), Buffer + 65536 + 84, sizeof(EFI_GUID));
            return;
         } // if
      } // search for ReiserFS magic

      if (BufferSize >= (65536 + 64 + 8)) {
         MagicString = (char*) (Buffer + 65536 + 64);
         if (CompareMem(MagicString, BTRFS_SIGNATURE, 8) == 0) {
            Volume->FSType = FS_TYPE_BTRFS;
            return;
         } // if
      } // search for Btrfs magic

      if (BufferSize >= 512) {
         MagicString = (char*) Buffer;
         if (CompareMem(MagicString, XFS_SIGNATURE, 4) == 0) {
            Volume->FSType = FS_TYPE_XFS;
            return;
         }
      } // search for XFS magic

      if (BufferSize >= (1024 + 2)) {
         Magic16 = (UINT16*) (Buffer + 1024);
         if ((*Magic16 == HFSPLUS_MAGIC1) || (*Magic16 == HFSPLUS_MAGIC2)) {
            Volume->FSType = FS_TYPE_HFSPLUS;
            return;
         }
      } // search for HFS+ magic

      if (BufferSize >= 512) {
         // Search for NTFS, FAT, and MBR/EBR.
         // These all have 0xAA55 at the end of the first sector, but FAT and
         // MBR/EBR are not easily distinguished. Thus, we first look for NTFS
         // "magic"; then check to see if the volume can be mounted, thus
         // relying on the EFI's built-in FAT driver to identify FAT; and then
         // check to see if the "volume" is in fact a whole-disk device.
         Magic16 = (UINT16*) (Buffer + 510);
         if (*Magic16 == FAT_MAGIC) {
            MagicString = (char*) (Buffer + 3);
            if (CompareMem(MagicString, NTFS_SIGNATURE, 8) == 0) {
               Volume->FSType = FS_TYPE_NTFS;
               CopyMem(&(Volume->VolUuid), Buffer + 0x48, sizeof(UINT64));
            } else {
               RootDir = LibOpenRoot(Volume->DeviceHandle);
               if (RootDir != NULL) {
                  Volume->FSType = FS_TYPE_FAT;
               } else if (!Volume->BlockIO->Media->LogicalPartition) {
                  Volume->FSType = FS_TYPE_WHOLEDISK;
               } // if/elseif/else
            } // if/else
            return;
         } // if
      } // search for FAT and NTFS magic

      // If no other filesystem is identified and block size is right, assume
      // it's ISO-9660....
      if (Volume->BlockIO->Media->BlockSize == 2048) {
          Volume->FSType = FS_TYPE_ISO9660;
          return;
      }

   } // if ((Buffer != NULL) && (Volume != NULL))

} // UINT32 SetFilesystemData()

static VOID ScanVolumeBootcode(REFIT_VOLUME *Volume, BOOLEAN *Bootable)
{
    EFI_STATUS              Status;
    UINT8                   Buffer[SAMPLE_SIZE];
    UINTN                   i;
    MBR_PARTITION_INFO      *MbrTable;
    BOOLEAN                 MbrTableFound = FALSE;

    Volume->HasBootCode = FALSE;
    Volume->OSIconName = NULL;
    Volume->OSName = NULL;
    *Bootable = FALSE;

    if (Volume->BlockIO == NULL)
        return;
    if (Volume->BlockIO->Media->BlockSize > SAMPLE_SIZE)
        return;   // our buffer is too small...

    // look at the boot sector (this is used for both hard disks and El Torito images!)
    Status = refit_call5_wrapper(Volume->BlockIO->ReadBlocks,
                                 Volume->BlockIO, Volume->BlockIO->Media->MediaId,
                                 Volume->BlockIOOffset, SAMPLE_SIZE, Buffer);
    if (!EFI_ERROR(Status)) {
        SetFilesystemData(Buffer, SAMPLE_SIZE, Volume);
    }
    if ((Status == EFI_SUCCESS) && (GlobalConfig.LegacyType == LEGACY_TYPE_MAC)) {
        if ((*((UINT16 *)(Buffer + 510)) == 0xaa55 && Buffer[0] != 0) && (FindMem(Buffer, 512, "EXFAT", 5) == -1)) {
            *Bootable = TRUE;
            Volume->HasBootCode = TRUE;
        }

        // detect specific boot codes
        if (CompareMem(Buffer + 2, "LILO", 4) == 0 ||
            CompareMem(Buffer + 6, "LILO", 4) == 0 ||
            CompareMem(Buffer + 3, "SYSLINUX", 8) == 0 ||
            FindMem(Buffer, SECTOR_SIZE, "ISOLINUX", 8) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"linux";
            Volume->OSName = L"Linux";

        } else if (FindMem(Buffer, 512, "Geom\0Hard Disk\0Read\0 Error", 26) >= 0) {   // GRUB
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"grub,linux";
            Volume->OSName = L"Linux";

        } else if ((*((UINT32 *)(Buffer + 502)) == 0 &&
                    *((UINT32 *)(Buffer + 506)) == 50000 &&
                    *((UINT16 *)(Buffer + 510)) == 0xaa55) ||
                    FindMem(Buffer, SECTOR_SIZE, "Starting the BTX loader", 23) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"freebsd";
            Volume->OSName = L"FreeBSD";

        // If more differentiation needed, also search for
        // "Invalid partition table" &/or "Missing boot loader".
        } else if ((*((UINT16 *)(Buffer + 510)) == 0xaa55) &&
                   (FindMem(Buffer, SECTOR_SIZE, "Boot loader too large", 21) >= 0) &&
                   (FindMem(Buffer, SECTOR_SIZE, "I/O error loading boot loader", 29) >= 0))  {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"freebsd";
            Volume->OSName = L"FreeBSD";

        } else if (FindMem(Buffer, 512, "!Loading", 8) >= 0 ||
                   FindMem(Buffer, SECTOR_SIZE, "/cdboot\0/CDBOOT\0", 16) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"openbsd";
            Volume->OSName = L"OpenBSD";

        } else if (FindMem(Buffer, 512, "Not a bootxx image", 18) >= 0 ||
                   *((UINT32 *)(Buffer + 1028)) == 0x7886b6d1) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"netbsd";
            Volume->OSName = L"NetBSD";

        // Windows NT/200x/XP
        } else if (FindMem(Buffer, SECTOR_SIZE, "NTLDR", 5) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"win";
            Volume->OSName = L"Windows";

        // Windows Vista/7/8
        } else if (FindMem(Buffer, SECTOR_SIZE, "BOOTMGR", 7) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"win8,win";
            Volume->OSName = L"Windows";

        } else if (FindMem(Buffer, 512, "CPUBOOT SYS", 11) >= 0 ||
                   FindMem(Buffer, 512, "KERNEL  SYS", 11) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"freedos";
            Volume->OSName = L"FreeDOS";

        } else if (FindMem(Buffer, 512, "OS2LDR", 6) >= 0 ||
                   FindMem(Buffer, 512, "OS2BOOT", 7) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"ecomstation";
            Volume->OSName = L"eComStation";

        } else if (FindMem(Buffer, 512, "Be Boot Loader", 14) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"beos";
            Volume->OSName = L"BeOS";

        } else if (FindMem(Buffer, 512, "yT Boot Loader", 14) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"zeta,beos";
            Volume->OSName = L"ZETA";

        } else if (FindMem(Buffer, 512, "\x04" "beos\x06" "system\x05" "zbeos", 18) >= 0 ||
                   FindMem(Buffer, 512, "\x06" "system\x0c" "haiku_loader", 20) >= 0) {
            Volume->HasBootCode = TRUE;
            Volume->OSIconName = L"haiku,beos";
            Volume->OSName = L"Haiku";

        }

        // NOTE: If you add an operating system with a name that starts with 'W' or 'L', you
        //  need to fix AddLegacyEntry in refind/legacy.c.

#if REFIT_DEBUG > 0
        Print(L"  Result of bootcode detection: %s %s (%s)\n",
              Volume->HasBootCode ? L"bootable" : L"non-bootable",
              Volume->OSName, Volume->OSIconName);
#endif

        // dummy FAT boot sector (created by OS X's newfs_msdos)
        if (FindMem(Buffer, 512, "Non-system disk", 15) >= 0)
            Volume->HasBootCode = FALSE;

        // dummy FAT boot sector (created by Linux's mkdosfs)
        if (FindMem(Buffer, 512, "This is not a bootable disk", 27) >= 0)
            Volume->HasBootCode = FALSE;

        // dummy FAT boot sector (created by Windows)
        if (FindMem(Buffer, 512, "Press any key to restart", 24) >= 0)
            Volume->HasBootCode = FALSE;

        // check for MBR partition table
        if (*((UINT16 *)(Buffer + 510)) == 0xaa55) {
            MbrTable = (MBR_PARTITION_INFO *)(Buffer + 446);
            for (i = 0; i < 4; i++)
                if (MbrTable[i].StartLBA && MbrTable[i].Size)
                    MbrTableFound = TRUE;
            for (i = 0; i < 4; i++)
                if (MbrTable[i].Flags != 0x00 && MbrTable[i].Flags != 0x80)
                    MbrTableFound = FALSE;
            if (MbrTableFound) {
                Volume->MbrPartitionTable = AllocatePool(4 * 16);
                CopyMem(Volume->MbrPartitionTable, MbrTable, 4 * 16);
            }
        }

    } else {
#if REFIT_DEBUG > 0
        CheckError(Status, L"while reading boot sector");
#endif
    }
} /* VOID ScanVolumeBootcode() */

// Set default volume badge icon based on /.VolumeBadge.{icns|png} file or disk kind
VOID SetVolumeBadgeIcon(REFIT_VOLUME *Volume)
{
   if (GlobalConfig.HideUIFlags & HIDEUI_FLAG_BADGES)
      return;

   if (Volume->VolBadgeImage == NULL) {
      Volume->VolBadgeImage = egLoadIconAnyType(Volume->RootDir, L"", L".VolumeBadge", GlobalConfig.IconSizes[ICON_SIZE_BADGE]);
   }

   if (Volume->VolBadgeImage == NULL) {
      switch (Volume->DiskKind) {
          case DISK_KIND_INTERNAL:
             Volume->VolBadgeImage = BuiltinIcon(BUILTIN_ICON_VOL_INTERNAL);
             break;
          case DISK_KIND_EXTERNAL:
             Volume->VolBadgeImage = BuiltinIcon(BUILTIN_ICON_VOL_EXTERNAL);
             break;
          case DISK_KIND_OPTICAL:
             Volume->VolBadgeImage = BuiltinIcon(BUILTIN_ICON_VOL_OPTICAL);
             break;
          case DISK_KIND_NET:
             Volume->VolBadgeImage = BuiltinIcon(BUILTIN_ICON_VOL_NET);
             break;
      } // switch()
   }
} // VOID SetVolumeBadgeIcon()

// Return a string representing the input size in IEEE-1541 units.
// The calling function is responsible for freeing the allocated memory.
static CHAR16 *SizeInIEEEUnits(UINT64 SizeInBytes) {
   UINT64 SizeInIeee;
   UINTN Index = 0, NumPrefixes;
   CHAR16 *Units, *Prefixes = L" KMGTPEZ";
   CHAR16 *TheValue;

   TheValue = AllocateZeroPool(sizeof(CHAR16) * 256);
   if (TheValue != NULL) {
      NumPrefixes = StrLen(Prefixes);
      SizeInIeee = SizeInBytes;
      while ((SizeInIeee > 1024) && (Index < (NumPrefixes - 1))) {
         Index++;
         SizeInIeee /= 1024;
      } // while
      if (Prefixes[Index] == ' ') {
         Units = StrDuplicate(L"-byte");
      } else {
         Units = StrDuplicate(L"  iB");
         Units[1] = Prefixes[Index];
      } // if/else
      SPrint(TheValue, 255, L"%ld%s", SizeInIeee, Units);
   } // if
   return TheValue;
} // CHAR16 *SizeInIEEEUnits()

// Return a name for the volume. Ideally this should be the label for the
// filesystem or volume, but this function falls back to describing the
// filesystem by size (200 MiB, etc.) and/or type (ext2, HFS+, etc.), if
// this information can be extracted.
// The calling function is responsible for freeing the memory allocated
// for the name string.
static CHAR16 *GetVolumeName(REFIT_VOLUME *Volume) {
   EFI_FILE_SYSTEM_INFO    *FileSystemInfoPtr = NULL;
   CHAR16                  *FoundName = NULL;
   CHAR16                  *SISize, *TypeName;

   if (Volume->RootDir != NULL) {
      FileSystemInfoPtr = LibFileSystemInfo(Volume->RootDir);
   }

   if ((FileSystemInfoPtr != NULL) && (FileSystemInfoPtr->VolumeLabel != NULL) &&
       (StrLen(FileSystemInfoPtr->VolumeLabel) > 0)) {
      FoundName = StrDuplicate(FileSystemInfoPtr->VolumeLabel);
   }

   // If no filesystem name, try to use the partition name....
   if ((FoundName == NULL) && (Volume->PartName != NULL) && (StrLen(Volume->PartName) > 0) &&
       !IsIn(Volume->PartName, IGNORE_PARTITION_NAMES)) {
      FoundName = StrDuplicate(Volume->PartName);
   } // if use partition name

   // No filesystem or acceptable partition name, so use fs type and size
   if ((FoundName == NULL) && (FileSystemInfoPtr != NULL)) {
      FoundName = AllocateZeroPool(sizeof(CHAR16) * 256);
      if (FoundName != NULL) {
         SISize = SizeInIEEEUnits(FileSystemInfoPtr->VolumeSize);
         SPrint(FoundName, 255, L"%s%s volume", SISize, FSTypeName(Volume->FSType));
         MyFreePool(SISize);
      } // if allocated memory OK
   } // if (FoundName == NULL)

   MyFreePool(FileSystemInfoPtr);

   if (FoundName == NULL) {
      FoundName = AllocateZeroPool(sizeof(CHAR16) * 256);
      if (FoundName != NULL) {
         TypeName = FSTypeName(Volume->FSType); // NOTE: Don't free TypeName; function returns constant
         if (StrLen(TypeName) > 0)
            SPrint(FoundName, 255, L"%s volume", TypeName);
         else
            SPrint(FoundName, 255, L"unknown volume");
      } // if allocated memory OK
   } // if

   // TODO: Above could be improved/extended, in case filesystem name is not found,
   // such as:
   //  - use or add disk/partition number (e.g., "(hd0,2)")

   // Desperate fallback name....
   if (FoundName == NULL) {
      FoundName = StrDuplicate(L"unknown volume");
   }
   return FoundName;
} // static CHAR16 *GetVolumeName()

// Determine the unique GUID, type code GUID, and name of the volume and store them.
static VOID SetPartGuidAndName(REFIT_VOLUME *Volume, EFI_DEVICE_PATH_PROTOCOL *DevicePath) {
   HARDDRIVE_DEVICE_PATH    *HdDevicePath;
   GPT_ENTRY                *PartInfo;

   if ((Volume == NULL) || (DevicePath == NULL))
      return;

   if ((DevicePath->Type == MEDIA_DEVICE_PATH) && (DevicePath->SubType == MEDIA_HARDDRIVE_DP)) {
      HdDevicePath = (HARDDRIVE_DEVICE_PATH*) DevicePath;
      if (HdDevicePath->SignatureType == SIGNATURE_TYPE_GUID) {
         Volume->PartGuid = *((EFI_GUID*) HdDevicePath->Signature);
         PartInfo = FindPartWithGuid(&(Volume->PartGuid));
         if (PartInfo) {
             Volume->PartName = StrDuplicate(PartInfo->name);
             CopyMem(&(Volume->PartTypeGuid), PartInfo->type_guid, sizeof(EFI_GUID));
             if (GuidsAreEqual (&(Volume->PartTypeGuid), &gFreedesktopRootGuid)) {
                GlobalConfig.DiscoveredRoot = Volume;
             } // if (GUIDs match)
         } // if (PartInfo exists)
      } // if (GPT disk)
   } // if (disk device)
} // VOID SetPartGuid()

// Return TRUE if NTFS boot files are found or if Volume is unreadable,
// FALSE otherwise. The idea is to weed out non-boot NTFS volumes from
// BIOS/legacy boot list on Macs. We can't assume NTFS will be readable,
// so return TRUE if it's unreadable; but if it IS readable, return
// TRUE only if Windows boot files are found.
static BOOLEAN HasWindowsBiosBootFiles(REFIT_VOLUME *Volume) {
   BOOLEAN FilesFound = TRUE;

   if (Volume->RootDir != NULL) {
      FilesFound = FileExists(Volume->RootDir, L"NTLDR") ||  // Windows NT/200x/XP boot file
                   FileExists(Volume->RootDir, L"bootmgr");  // Windows Vista/7/8 boot file
   } // if
   return FilesFound;
} // static VOID HasWindowsBiosBootFiles()

VOID ScanVolume(REFIT_VOLUME *Volume)
{
    EFI_STATUS              Status;
    EFI_DEVICE_PATH         *DevicePath, *NextDevicePath;
    EFI_DEVICE_PATH         *DiskDevicePath, *RemainingDevicePath;
    EFI_HANDLE              WholeDiskHandle;
    UINTN                   PartialLength;
    BOOLEAN                 Bootable;

    // get device path
    Volume->DevicePath = DuplicateDevicePath(DevicePathFromHandle(Volume->DeviceHandle));
#if REFIT_DEBUG > 0
    if (Volume->DevicePath != NULL) {
        Print(L"* %s\n", DevicePathToStr(Volume->DevicePath));
#if REFIT_DEBUG >= 2
        DumpHex(1, 0, DevicePathSize(Volume->DevicePath), Volume->DevicePath);
#endif
    }
#endif

    Volume->DiskKind = DISK_KIND_INTERNAL;  // default

    // get block i/o
    Status = refit_call3_wrapper(BS->HandleProtocol, Volume->DeviceHandle, &BlockIoProtocol, (VOID **) &(Volume->BlockIO));
    if (EFI_ERROR(Status)) {
        Volume->BlockIO = NULL;
        Print(L"Warning: Can't get BlockIO protocol.\n");
    } else {
        if (Volume->BlockIO->Media->BlockSize == 2048)
            Volume->DiskKind = DISK_KIND_OPTICAL;
    }

    // scan for bootcode and MBR table
    Bootable = FALSE;
    ScanVolumeBootcode(Volume, &Bootable);

    // detect device type
    DevicePath = Volume->DevicePath;
    while (DevicePath != NULL && !IsDevicePathEndType(DevicePath)) {
        NextDevicePath = NextDevicePathNode(DevicePath);

        if (DevicePathType(DevicePath) == MEDIA_DEVICE_PATH) {
           SetPartGuidAndName(Volume, DevicePath);
        }
        if (DevicePathType(DevicePath) == MESSAGING_DEVICE_PATH &&
            (DevicePathSubType(DevicePath) == MSG_USB_DP ||
             DevicePathSubType(DevicePath) == MSG_USB_CLASS_DP ||
             DevicePathSubType(DevicePath) == MSG_1394_DP ||
             DevicePathSubType(DevicePath) == MSG_FIBRECHANNEL_DP))
            Volume->DiskKind = DISK_KIND_EXTERNAL;    // USB/FireWire/FC device -> external
        if (DevicePathType(DevicePath) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(DevicePath) == MEDIA_CDROM_DP) {
            Volume->DiskKind = DISK_KIND_OPTICAL;     // El Torito entry -> optical disk
            Bootable = TRUE;
        }

        if (DevicePathType(DevicePath) == MEDIA_DEVICE_PATH && DevicePathSubType(DevicePath) == MEDIA_VENDOR_DP) {
            Volume->IsAppleLegacy = TRUE;             // legacy BIOS device entry
            // TODO: also check for Boot Camp GUID
            Bootable = FALSE;   // this handle's BlockIO is just an alias for the whole device
        }

        if (DevicePathType(DevicePath) == MESSAGING_DEVICE_PATH) {
            // make a device path for the whole device
            PartialLength = (UINT8 *)NextDevicePath - (UINT8 *)(Volume->DevicePath);
            DiskDevicePath = (EFI_DEVICE_PATH *)AllocatePool(PartialLength + sizeof(EFI_DEVICE_PATH));
            CopyMem(DiskDevicePath, Volume->DevicePath, PartialLength);
            CopyMem((UINT8 *)DiskDevicePath + PartialLength, EndDevicePath, sizeof(EFI_DEVICE_PATH));

            // get the handle for that path
            RemainingDevicePath = DiskDevicePath;
            Status = refit_call3_wrapper(BS->LocateDevicePath, &BlockIoProtocol, &RemainingDevicePath, &WholeDiskHandle);
            FreePool(DiskDevicePath);

            if (!EFI_ERROR(Status)) {
                //Print(L"  - original handle: %08x - disk handle: %08x\n", (UINT32)DeviceHandle, (UINT32)WholeDiskHandle);

                // get the device path for later
                Status = refit_call3_wrapper(BS->HandleProtocol, WholeDiskHandle, &DevicePathProtocol, (VOID **) &DiskDevicePath);
                if (!EFI_ERROR(Status)) {
                    Volume->WholeDiskDevicePath = DuplicateDevicePath(DiskDevicePath);
                }

                // look at the BlockIO protocol
                Status = refit_call3_wrapper(BS->HandleProtocol, WholeDiskHandle, &BlockIoProtocol,
                                             (VOID **) &Volume->WholeDiskBlockIO);
                if (!EFI_ERROR(Status)) {

                    // check the media block size
                    if (Volume->WholeDiskBlockIO->Media->BlockSize == 2048)
                        Volume->DiskKind = DISK_KIND_OPTICAL;

                } else {
                    Volume->WholeDiskBlockIO = NULL;
                    //CheckError(Status, L"from HandleProtocol");
                }
            } //else
              //  CheckError(Status, L"from LocateDevicePath");
        }

        DevicePath = NextDevicePath;
    } // while

   if (!Bootable) {
#if REFIT_DEBUG > 0
      if (Volume->HasBootCode)
         Print(L"  Volume considered non-bootable, but boot code is present\n");
#endif
      Volume->HasBootCode = FALSE;
   }

   // open the root directory of the volume
   Volume->RootDir = LibOpenRoot(Volume->DeviceHandle);

   // Set volume icon based on .VolumeBadge icon or disk kind
   SetVolumeBadgeIcon(Volume);

   Volume->VolName = GetVolumeName(Volume);

   if (Volume->RootDir == NULL) {
      Volume->IsReadable = FALSE;
      return;
   } else {
      Volume->IsReadable = TRUE;
      if ((GlobalConfig.LegacyType == LEGACY_TYPE_MAC) && (Volume->FSType == FS_TYPE_NTFS) && Volume->HasBootCode) {
         // VBR boot code found on NTFS, but volume is not actually bootable
         // unless there are actual boot file, so check for them....
         Volume->HasBootCode = HasWindowsBiosBootFiles(Volume);
      }
   } // if/else

   // get custom volume icons if present
   if (!Volume->VolIconImage) {
      Volume->VolIconImage = egLoadIconAnyType(Volume->RootDir, L"", L".VolumeIcon", GlobalConfig.IconSizes[ICON_SIZE_BIG]);
   }
} // ScanVolume()

static VOID ScanExtendedPartition(REFIT_VOLUME *WholeDiskVolume, MBR_PARTITION_INFO *MbrEntry)
{
    EFI_STATUS              Status;
    REFIT_VOLUME            *Volume;
    UINT32                  ExtBase, ExtCurrent, NextExtCurrent;
    UINTN                   i;
    UINTN                   LogicalPartitionIndex = 4;
    UINT8                   SectorBuffer[512];
    BOOLEAN                 Bootable;
    MBR_PARTITION_INFO      *EMbrTable;

    ExtBase = MbrEntry->StartLBA;

    for (ExtCurrent = ExtBase; ExtCurrent; ExtCurrent = NextExtCurrent) {
        // read current EMBR
      Status = refit_call5_wrapper(WholeDiskVolume->BlockIO->ReadBlocks,
                                   WholeDiskVolume->BlockIO,
                                   WholeDiskVolume->BlockIO->Media->MediaId,
                                   ExtCurrent, 512, SectorBuffer);
        if (EFI_ERROR(Status))
            break;
        if (*((UINT16 *)(SectorBuffer + 510)) != 0xaa55)
            break;
        EMbrTable = (MBR_PARTITION_INFO *)(SectorBuffer + 446);

        // scan logical partitions in this EMBR
        NextExtCurrent = 0;
        for (i = 0; i < 4; i++) {
            if ((EMbrTable[i].Flags != 0x00 && EMbrTable[i].Flags != 0x80) ||
                EMbrTable[i].StartLBA == 0 || EMbrTable[i].Size == 0)
                break;
            if (IS_EXTENDED_PART_TYPE(EMbrTable[i].Type)) {
                // set next ExtCurrent
                NextExtCurrent = ExtBase + EMbrTable[i].StartLBA;
                break;
            } else {

                // found a logical partition
                Volume = AllocateZeroPool(sizeof(REFIT_VOLUME));
                Volume->DiskKind = WholeDiskVolume->DiskKind;
                Volume->IsMbrPartition = TRUE;
                Volume->MbrPartitionIndex = LogicalPartitionIndex++;
                Volume->VolName = AllocateZeroPool(256 * sizeof(UINT16));
                SPrint(Volume->VolName, 255, L"Partition %d", Volume->MbrPartitionIndex + 1);
                Volume->BlockIO = WholeDiskVolume->BlockIO;
                Volume->BlockIOOffset = ExtCurrent + EMbrTable[i].StartLBA;
                Volume->WholeDiskBlockIO = WholeDiskVolume->BlockIO;

                Bootable = FALSE;
                ScanVolumeBootcode(Volume, &Bootable);
                if (!Bootable)
                    Volume->HasBootCode = FALSE;

                SetVolumeBadgeIcon(Volume);

                AddListElement((VOID ***) &Volumes, &VolumesCount, Volume);

            }
        }
    }
} /* VOID ScanExtendedPartition() */

VOID ScanVolumes(VOID)
{
    EFI_STATUS              Status;
    EFI_HANDLE              *Handles;
    REFIT_VOLUME            *Volume, *WholeDiskVolume;
    MBR_PARTITION_INFO      *MbrTable;
    UINTN                   HandleCount = 0;
    UINTN                   HandleIndex;
    UINTN                   VolumeIndex, VolumeIndex2;
    UINTN                   PartitionIndex;
    UINTN                   SectorSum, i, VolNumber = 0;
    UINT8                   *SectorBuffer1, *SectorBuffer2;
    EFI_GUID                *UuidList;
    EFI_GUID                NullUuid = NULL_GUID_VALUE;

    MyFreePool(Volumes);
    Volumes = NULL;
    VolumesCount = 0;
    ForgetPartitionTables();

    // get all filesystem handles
    Status = LibLocateHandle(ByProtocol, &BlockIoProtocol, NULL, &HandleCount, &Handles);
    UuidList = AllocateZeroPool(sizeof(EFI_GUID) * HandleCount);
    if (Status == EFI_NOT_FOUND) {
        return;  // no filesystems. strange, but true...
    }
    if (CheckError(Status, L"while listing all file systems"))
        return;

    // first pass: collect information about all handles
    for (HandleIndex = 0; HandleIndex < HandleCount; HandleIndex++) {
        Volume = AllocateZeroPool(sizeof(REFIT_VOLUME));
        Volume->DeviceHandle = Handles[HandleIndex];
        AddPartitionTable(Volume);
        ScanVolume(Volume);
        if (UuidList) {
           UuidList[HandleIndex] = Volume->VolUuid;
           for (i = 0; i < HandleIndex; i++) {
              if ((CompareMem(&(Volume->VolUuid), &(UuidList[i]), sizeof(EFI_GUID)) == 0) &&
                  (CompareMem(&(Volume->VolUuid), &NullUuid, sizeof(EFI_GUID)) != 0)) { // Duplicate filesystem UUID
                 Volume->IsReadable = FALSE;
              } // if
           } // for
        } // if
        if (Volume->IsReadable)
           Volume->VolNumber = VolNumber++;
        else
           Volume->VolNumber = VOL_UNREADABLE;

        AddListElement((VOID ***) &Volumes, &VolumesCount, Volume);

        if (Volume->DeviceHandle == SelfLoadedImage->DeviceHandle)
            SelfVolume = Volume;
    }
    MyFreePool(Handles);

    if (SelfVolume == NULL)
        Print(L"WARNING: SelfVolume not found");

    // second pass: relate partitions and whole disk devices
    for (VolumeIndex = 0; VolumeIndex < VolumesCount; VolumeIndex++) {
        Volume = Volumes[VolumeIndex];
        // check MBR partition table for extended partitions
        if (Volume->BlockIO != NULL && Volume->WholeDiskBlockIO != NULL &&
            Volume->BlockIO == Volume->WholeDiskBlockIO && Volume->BlockIOOffset == 0 &&
            Volume->MbrPartitionTable != NULL) {
            MbrTable = Volume->MbrPartitionTable;
            for (PartitionIndex = 0; PartitionIndex < 4; PartitionIndex++) {
                if (IS_EXTENDED_PART_TYPE(MbrTable[PartitionIndex].Type)) {
                   ScanExtendedPartition(Volume, MbrTable + PartitionIndex);
                }
            }
        }

        // search for corresponding whole disk volume entry
        WholeDiskVolume = NULL;
        if (Volume->BlockIO != NULL && Volume->WholeDiskBlockIO != NULL &&
            Volume->BlockIO != Volume->WholeDiskBlockIO) {
            for (VolumeIndex2 = 0; VolumeIndex2 < VolumesCount; VolumeIndex2++) {
                if (Volumes[VolumeIndex2]->BlockIO == Volume->WholeDiskBlockIO &&
                    Volumes[VolumeIndex2]->BlockIOOffset == 0) {
                    WholeDiskVolume = Volumes[VolumeIndex2];
                }
            }
        }

        if (WholeDiskVolume != NULL && WholeDiskVolume->MbrPartitionTable != NULL) {
            // check if this volume is one of the partitions in the table
            MbrTable = WholeDiskVolume->MbrPartitionTable;
            SectorBuffer1 = AllocatePool(512);
            SectorBuffer2 = AllocatePool(512);
            for (PartitionIndex = 0; PartitionIndex < 4; PartitionIndex++) {
                // check size
                if ((UINT64)(MbrTable[PartitionIndex].Size) != Volume->BlockIO->Media->LastBlock + 1)
                    continue;

                // compare boot sector read through offset vs. directly
                Status = refit_call5_wrapper(Volume->BlockIO->ReadBlocks,
                                             Volume->BlockIO, Volume->BlockIO->Media->MediaId,
                                             Volume->BlockIOOffset, 512, SectorBuffer1);
                if (EFI_ERROR(Status))
                    break;
                Status = refit_call5_wrapper(Volume->WholeDiskBlockIO->ReadBlocks,
                                             Volume->WholeDiskBlockIO, Volume->WholeDiskBlockIO->Media->MediaId,
                                             MbrTable[PartitionIndex].StartLBA, 512, SectorBuffer2);
                if (EFI_ERROR(Status))
                    break;
                if (CompareMem(SectorBuffer1, SectorBuffer2, 512) != 0)
                    continue;
                SectorSum = 0;
                for (i = 0; i < 512; i++)
                    SectorSum += SectorBuffer1[i];
                if (SectorSum < 1000)
                    continue;

                // TODO: mark entry as non-bootable if it is an extended partition

                // now we're reasonably sure the association is correct...
                Volume->IsMbrPartition = TRUE;
                Volume->MbrPartitionIndex = PartitionIndex;
                if (Volume->VolName == NULL) {
                    Volume->VolName = AllocateZeroPool(sizeof(CHAR16) * 256);
                    SPrint(Volume->VolName, 255, L"Partition %d", PartitionIndex + 1);
                }
                break;
            }

            MyFreePool(SectorBuffer1);
            MyFreePool(SectorBuffer2);
        }
    } // for
} /* VOID ScanVolumes() */

static VOID UninitVolumes(VOID)
{
    REFIT_VOLUME            *Volume;
    UINTN                   VolumeIndex;

    for (VolumeIndex = 0; VolumeIndex < VolumesCount; VolumeIndex++) {
        Volume = Volumes[VolumeIndex];

        if (Volume->RootDir != NULL) {
            refit_call1_wrapper(Volume->RootDir->Close, Volume->RootDir);
            Volume->RootDir = NULL;
        }

        Volume->DeviceHandle = NULL;
        Volume->BlockIO = NULL;
        Volume->WholeDiskBlockIO = NULL;
    }
}

VOID ReinitVolumes(VOID)
{
    EFI_STATUS              Status;
    REFIT_VOLUME            *Volume;
    UINTN                   VolumeIndex;
    EFI_DEVICE_PATH         *RemainingDevicePath;
    EFI_HANDLE              DeviceHandle, WholeDiskHandle;

    for (VolumeIndex = 0; VolumeIndex < VolumesCount; VolumeIndex++) {
        Volume = Volumes[VolumeIndex];

        if (Volume->DevicePath != NULL) {
            // get the handle for that path
            RemainingDevicePath = Volume->DevicePath;
            Status = refit_call3_wrapper(BS->LocateDevicePath, &BlockIoProtocol, &RemainingDevicePath, &DeviceHandle);

            if (!EFI_ERROR(Status)) {
                Volume->DeviceHandle = DeviceHandle;

                // get the root directory
                Volume->RootDir = LibOpenRoot(Volume->DeviceHandle);

            } else
                CheckError(Status, L"from LocateDevicePath");
        }

        if (Volume->WholeDiskDevicePath != NULL) {
            // get the handle for that path
            RemainingDevicePath = Volume->WholeDiskDevicePath;
            Status = refit_call3_wrapper(BS->LocateDevicePath, &BlockIoProtocol, &RemainingDevicePath, &WholeDiskHandle);

            if (!EFI_ERROR(Status)) {
                // get the BlockIO protocol
                Status = refit_call3_wrapper(BS->HandleProtocol, WholeDiskHandle, &BlockIoProtocol,
                                             (VOID **) &Volume->WholeDiskBlockIO);
                if (EFI_ERROR(Status)) {
                    Volume->WholeDiskBlockIO = NULL;
                    CheckError(Status, L"from HandleProtocol");
                }
            } else
                CheckError(Status, L"from LocateDevicePath");
        }
    }
}

//
// file and dir functions
//

BOOLEAN FileExists(IN EFI_FILE *BaseDir, IN CHAR16 *RelativePath)
{
   EFI_STATUS         Status;
   EFI_FILE_HANDLE    TestFile;

   if (BaseDir != NULL) {
      Status = refit_call5_wrapper(BaseDir->Open, BaseDir, &TestFile, RelativePath, EFI_FILE_MODE_READ, 0);
      if (Status == EFI_SUCCESS) {
         refit_call1_wrapper(TestFile->Close, TestFile);
         return TRUE;
      }
   }
   return FALSE;
}

EFI_STATUS DirNextEntry(IN EFI_FILE *Directory, IN OUT EFI_FILE_INFO **DirEntry, IN UINTN FilterMode)
{
    EFI_STATUS Status;
    VOID *Buffer;
    UINTN LastBufferSize, BufferSize;
    INTN IterCount;

    for (;;) {

        // free pointer from last call
        if (*DirEntry != NULL) {
           FreePool(*DirEntry);
           *DirEntry = NULL;
        }

        // read next directory entry
        LastBufferSize = BufferSize = 256;
        Buffer = AllocatePool(BufferSize);
        for (IterCount = 0; ; IterCount++) {
            Status = refit_call3_wrapper(Directory->Read, Directory, &BufferSize, Buffer);
            if (Status != EFI_BUFFER_TOO_SMALL || IterCount >= 4)
                break;
            if (BufferSize <= LastBufferSize) {
                Print(L"FS Driver requests bad buffer size %d (was %d), using %d instead\n", BufferSize, LastBufferSize, LastBufferSize * 2);
                BufferSize = LastBufferSize * 2;
#if REFIT_DEBUG > 0
            } else {
                Print(L"Reallocating buffer from %d to %d\n", LastBufferSize, BufferSize);
#endif
            }
            Buffer = EfiReallocatePool(Buffer, LastBufferSize, BufferSize);
            LastBufferSize = BufferSize;
        }
        if (EFI_ERROR(Status)) {
            MyFreePool(Buffer);
            Buffer = NULL;
            break;
        }

        // check for end of listing
        if (BufferSize == 0) {    // end of directory listing
            MyFreePool(Buffer);
            Buffer = NULL;
            break;
        }

        // entry is ready to be returned
        *DirEntry = (EFI_FILE_INFO *)Buffer;

        // filter results
        if (FilterMode == 1) {   // only return directories
            if (((*DirEntry)->Attribute & EFI_FILE_DIRECTORY))
                break;
        } else if (FilterMode == 2) {   // only return files
            if (((*DirEntry)->Attribute & EFI_FILE_DIRECTORY) == 0)
                break;
        } else                   // no filter or unknown filter -> return everything
            break;

    }
    return Status;
}

VOID DirIterOpen(IN EFI_FILE *BaseDir, IN CHAR16 *RelativePath OPTIONAL, OUT REFIT_DIR_ITER *DirIter)
{
    if (RelativePath == NULL) {
        DirIter->LastStatus = EFI_SUCCESS;
        DirIter->DirHandle = BaseDir;
        DirIter->CloseDirHandle = FALSE;
    } else {
        DirIter->LastStatus = refit_call5_wrapper(BaseDir->Open, BaseDir, &(DirIter->DirHandle), RelativePath, EFI_FILE_MODE_READ, 0);
        DirIter->CloseDirHandle = EFI_ERROR(DirIter->LastStatus) ? FALSE : TRUE;
    }
    DirIter->LastFileInfo = NULL;
}

#ifndef __MAKEWITH_GNUEFI
EFI_UNICODE_COLLATION_PROTOCOL *mUnicodeCollation = NULL;

static EFI_STATUS
InitializeUnicodeCollationProtocol (VOID)
{
   EFI_STATUS  Status;

   if (mUnicodeCollation != NULL) {
      return EFI_SUCCESS;
   }

   //
   // BUGBUG: Proper impelmentation is to locate all Unicode Collation Protocol
   // instances first and then select one which support English language.
   // Current implementation just pick the first instance.
   //
   Status = gBS->LocateProtocol (
                          &gEfiUnicodeCollation2ProtocolGuid,
                          NULL,
                          (VOID **) &mUnicodeCollation
                          );
  if (EFI_ERROR(Status)) {
    Status = gBS->LocateProtocol (
                  &gEfiUnicodeCollationProtocolGuid,
                  NULL,
                  (VOID **) &mUnicodeCollation
                  );

  }
   return Status;
}

static BOOLEAN
MetaiMatch (IN CHAR16 *String, IN CHAR16 *Pattern)
{
   if (!mUnicodeCollation) {
      InitializeUnicodeCollationProtocol();
   }
   if (mUnicodeCollation)
      return mUnicodeCollation->MetaiMatch (mUnicodeCollation, String, Pattern);
   return FALSE; // Shouldn't happen
}

#endif

BOOLEAN DirIterNext(IN OUT REFIT_DIR_ITER *DirIter, IN UINTN FilterMode, IN CHAR16 *FilePattern OPTIONAL,
                    OUT EFI_FILE_INFO **DirEntry)
{
    BOOLEAN KeepGoing = TRUE;
    UINTN   i;
    CHAR16  *OnePattern;

    if (DirIter->LastFileInfo != NULL) {
       FreePool(DirIter->LastFileInfo);
       DirIter->LastFileInfo = NULL;
    }

    if (EFI_ERROR(DirIter->LastStatus))
        return FALSE;   // stop iteration

    do {
        DirIter->LastStatus = DirNextEntry(DirIter->DirHandle, &(DirIter->LastFileInfo), FilterMode);
        if (EFI_ERROR(DirIter->LastStatus))
           return FALSE;
        if (DirIter->LastFileInfo == NULL)  // end of listing
            return FALSE;
        if (FilePattern != NULL) {
            if ((DirIter->LastFileInfo->Attribute & EFI_FILE_DIRECTORY))
                KeepGoing = FALSE;
            i = 0;
            while (KeepGoing && (OnePattern = FindCommaDelimited(FilePattern, i++)) != NULL) {
               if (MetaiMatch(DirIter->LastFileInfo->FileName, OnePattern))
                   KeepGoing = FALSE;
            } // while
            // else continue loop
        } else
            break;
   } while (KeepGoing && FilePattern);

    *DirEntry = DirIter->LastFileInfo;
    return TRUE;
}

EFI_STATUS DirIterClose(IN OUT REFIT_DIR_ITER *DirIter)
{
   if (DirIter->LastFileInfo != NULL) {
      FreePool(DirIter->LastFileInfo);
      DirIter->LastFileInfo = NULL;
   }
   if (DirIter->CloseDirHandle)
      refit_call1_wrapper(DirIter->DirHandle->Close, DirIter->DirHandle);
   return DirIter->LastStatus;
}

//
// file name manipulation
//

// Returns the filename portion (minus path name) of the
// specified file
CHAR16 * Basename(IN CHAR16 *Path)
{
    CHAR16  *FileName;
    UINTN   i;

    FileName = Path;

    if (Path != NULL) {
        for (i = StrLen(Path); i > 0; i--) {
            if (Path[i-1] == '\\' || Path[i-1] == '/') {
                FileName = Path + i;
                break;
            }
        }
    }

    return FileName;
}

// Remove the .efi extension from FileName -- for instance, if FileName is
// "fred.efi", returns "fred". If the filename contains no .efi extension,
// returns a copy of the original input.
CHAR16 * StripEfiExtension(CHAR16 *FileName) {
   UINTN  Length;
   CHAR16 *Copy = NULL;

   if ((FileName != NULL) && ((Copy = StrDuplicate(FileName)) != NULL)) {
      Length = StrLen(Copy);
      if ((Length >= 4) && MyStriCmp(&Copy[Length - 4], L".efi")) {
         Copy[Length - 4] = 0;
      } // if
   } // if
   return Copy;
} // CHAR16 * StripExtension()

//
// memory string search
//

INTN FindMem(IN VOID *Buffer, IN UINTN BufferLength, IN VOID *SearchString, IN UINTN SearchStringLength)
{
    UINT8 *BufferPtr;
    UINTN Offset;

    BufferPtr = Buffer;
    BufferLength -= SearchStringLength;
    for (Offset = 0; Offset < BufferLength; Offset++, BufferPtr++) {
        if (CompareMem(BufferPtr, SearchString, SearchStringLength) == 0)
            return (INTN)Offset;
    }

    return -1;
}

BOOLEAN StriSubCmp(IN CHAR16 *SmallStr, IN CHAR16 *BigStr) {
    BOOLEAN Found = 0, Terminate = 0;
    UINTN BigIndex = 0, SmallIndex = 0, BigStart = 0;

    if (SmallStr && BigStr) {
        while (!Terminate) {
            if (BigStr[BigIndex] == '\0') {
                Terminate = 1;
            }
            if (SmallStr[SmallIndex] == '\0') {
                Found = 1;
                Terminate = 1;
            }
            if ((SmallStr[SmallIndex] & ~0x20) == (BigStr[BigIndex] & ~0x20)) {
                SmallIndex++;
                BigIndex++;
            } else {
                SmallIndex = 0;
                BigStart++;
                BigIndex = BigStart;
            }
        } // while
    } // if
    return Found;
} // BOOLEAN StriSubCmp()

// Performs a case-insensitive string comparison. This function is necesary
// because some EFIs have buggy StriCmp() functions that actually perform
// case-sensitive comparisons.
// Returns TRUE if strings are identical, FALSE otherwise.
BOOLEAN MyStriCmp(IN CONST CHAR16 *FirstString, IN CONST CHAR16 *SecondString) {
    if (FirstString && SecondString) {
        while ((*FirstString != L'\0') && ((*FirstString & ~0x20) == (*SecondString & ~0x20))) {
                FirstString++;
                SecondString++;
        }
        return (*FirstString == *SecondString);
    } else {
        return FALSE;
    }
} // BOOLEAN MyStriCmp()

// Convert input string to all-lowercase.
// DO NOT USE the standard StrLwr() function, since it's broken on some EFIs!
VOID ToLower(CHAR16 * MyString) {
    UINTN i = 0;

    if (MyString) {
        while (MyString[i] != L'\0') {
            if ((MyString[i] >= L'A') && (MyString[i] <= L'Z'))
                MyString[i] = MyString[i] - L'A' + L'a';
            i++;
        } // while
    } // if
} // VOID ToLower()

// Merges two strings, creating a new one and returning a pointer to it.
// If AddChar != 0, the specified character is placed between the two original
// strings (unless the first string is NULL or empty). The original input
// string *First is de-allocated and replaced by the new merged string.
// This is similar to StrCat, but safer and more flexible because
// MergeStrings allocates memory that's the correct size for the
// new merged string, so it can take a NULL *First and it cleans
// up the old memory. It should *NOT* be used with a constant
// *First, though....
VOID MergeStrings(IN OUT CHAR16 **First, IN CHAR16 *Second, CHAR16 AddChar) {
   UINTN Length1 = 0, Length2 = 0;
   CHAR16* NewString;

   if (*First != NULL)
      Length1 = StrLen(*First);
   if (Second != NULL)
      Length2 = StrLen(Second);
   NewString = AllocatePool(sizeof(CHAR16) * (Length1 + Length2 + 2));
   if (NewString != NULL) {
      if ((*First != NULL) && (Length1 == 0)) {
         MyFreePool(*First);
         *First = NULL;
      }
      NewString[0] = L'\0';
      if (*First != NULL) {
         StrCat(NewString, *First);
         if (AddChar) {
            NewString[Length1] = AddChar;
            NewString[Length1 + 1] = '\0';
         } // if (AddChar)
      } // if (*First != NULL)
      if (Second != NULL)
         StrCat(NewString, Second);
      MyFreePool(*First);
      *First = NewString;
   } else {
      Print(L"Error! Unable to allocate memory in MergeStrings()!\n");
   } // if/else
} // VOID MergeStrings()

// Similar to MergeStrings, but breaks the input string into word chunks and
// merges each word separately. Words are defined as string fragments separated
// by ' ', '_', or '-'.
VOID MergeWords(CHAR16 **MergeTo, CHAR16 *SourceString, CHAR16 AddChar) {
    CHAR16 *Temp, *Word, *p;
    BOOLEAN LineFinished = FALSE;

    if (SourceString) {
        Temp = Word = p = StrDuplicate(SourceString);
        if (Temp) {
            while (!LineFinished) {
                if ((*p == L' ') || (*p == L'_') || (*p == L'-') || (*p == L'\0')) {
                    if (*p == L'\0')
                        LineFinished = TRUE;
                    *p = L'\0';
                    if (*Word != L'\0')
                        MergeStrings(MergeTo, Word, AddChar);
                    Word = p + 1;
                } // if
                p++;
            } // while
            MyFreePool(Temp);
        } else {
            Print(L"Error! Unable to allocate memory in MergeWords()!\n");
        } // if/else
    } // if
} // VOID MergeWords()

// Takes an input pathname (*Path) and returns the part of the filename from
// the final dot onwards, converted to lowercase. If the filename includes
// no dots, or if the input is NULL, returns an empty (but allocated) string.
// The calling function is responsible for freeing the memory associated with
// the return value.
CHAR16 *FindExtension(IN CHAR16 *Path) {
   CHAR16     *Extension;
   BOOLEAN    Found = FALSE, FoundSlash = FALSE;
   INTN       i;

   Extension = AllocateZeroPool(sizeof(CHAR16));
   if (Path) {
      i = StrLen(Path);
      while ((!Found) && (!FoundSlash) && (i >= 0)) {
         if (Path[i] == L'.')
            Found = TRUE;
         else if ((Path[i] == L'/') || (Path[i] == L'\\'))
            FoundSlash = TRUE;
         if (!Found)
            i--;
      } // while
      if (Found) {
         MergeStrings(&Extension, &Path[i], 0);
         ToLower(Extension);
      } // if (Found)
   } // if
   return (Extension);
} // CHAR16 *FindExtension

// Takes an input pathname (*Path) and locates the final directory component
// of that name. For instance, if the input path is 'EFI\foo\bar.efi', this
// function returns the string 'foo'.
// Assumes the pathname is separated with backslashes.
CHAR16 *FindLastDirName(IN CHAR16 *Path) {
   UINTN i, StartOfElement = 0, EndOfElement = 0, PathLength, CopyLength;
   CHAR16 *Found = NULL;

   if (Path == NULL)
      return NULL;

   PathLength = StrLen(Path);
   // Find start & end of target element
   for (i = 0; i < PathLength; i++) {
      if (Path[i] == '\\') {
         StartOfElement = EndOfElement;
         EndOfElement = i;
      } // if
   } // for
   // Extract the target element
   if (EndOfElement > 0) {
      while ((StartOfElement < PathLength) && (Path[StartOfElement] == '\\')) {
         StartOfElement++;
      } // while
      EndOfElement--;
      if (EndOfElement >= StartOfElement) {
         CopyLength = EndOfElement - StartOfElement + 1;
         Found = StrDuplicate(&Path[StartOfElement]);
         if (Found != NULL)
            Found[CopyLength] = 0;
      } // if (EndOfElement >= StartOfElement)
   } // if (EndOfElement > 0)
   return (Found);
} // CHAR16 *FindLastDirName

// Returns the directory portion of a pathname. For instance,
// if FullPath is 'EFI\foo\bar.efi', this function returns the
// string 'EFI\foo'. The calling function is responsible for
// freeing the returned string's memory.
CHAR16 *FindPath(IN CHAR16* FullPath) {
   UINTN i, LastBackslash = 0;
   CHAR16 *PathOnly = NULL;

   if (FullPath != NULL) {
      for (i = 0; i < StrLen(FullPath); i++) {
         if (FullPath[i] == '\\')
            LastBackslash = i;
      } // for
      PathOnly = StrDuplicate(FullPath);
      if (PathOnly != NULL)
         PathOnly[LastBackslash] = 0;
   } // if
   return (PathOnly);
}

/*++
 * 
 * Routine Description:
 *
 *  Find a substring.
 *
 * Arguments: 
 *
 *  String      - Null-terminated string to search.
 *  StrCharSet  - Null-terminated string to search for.
 *
 * Returns:
 *  The address of the first occurrence of the matching substring if successful, or NULL otherwise.
 * --*/
CHAR16* MyStrStr (CHAR16  *String, CHAR16  *StrCharSet)
{
   CHAR16 *Src;
   CHAR16 *Sub;

   if ((String == NULL) || (StrCharSet == NULL))
      return NULL;

   Src = String;
   Sub = StrCharSet;

   while ((*String != L'\0') && (*StrCharSet != L'\0')) {
      if (*String++ != *StrCharSet) {
         String = ++Src;
         StrCharSet = Sub;
      } else {
         StrCharSet++;
      }
   }
   if (*StrCharSet == L'\0') {
      return Src;
   } else {
      return NULL;
   }
} // CHAR16 *MyStrStr()

// Restrict TheString to at most Limit characters.
// Does this in two ways:
// - Locates stretches of two or more spaces and compresses
//   them down to one space.
// - Truncates TheString
// Returns TRUE if changes were made, FALSE otherwise
BOOLEAN LimitStringLength(CHAR16 *TheString, UINTN Limit) {
   CHAR16    *SubString, *TempString;
   UINTN     i;
   BOOLEAN   HasChanged = FALSE;

   // SubString will be NULL or point WITHIN TheString
   SubString = MyStrStr(TheString, L"  ");
   while (SubString != NULL) {
      i = 0;
      while (SubString[i] == L' ')
         i++;
      if (i >= StrLen(SubString)) {
         SubString[0] = '\0';
         HasChanged = TRUE;
      } else {
         TempString = StrDuplicate(&SubString[i]);
         if (TempString != NULL) {
            StrCpy(&SubString[1], TempString);
            MyFreePool(TempString);
            HasChanged = TRUE;
         } else {
            // memory allocation problem; abort to avoid potentially infinite loop!
            break;
         } // if/else
      } // if/else
      SubString = MyStrStr(TheString, L"  ");
   } // while

   // If the string is still too long, truncate it....
   if (StrLen(TheString) > Limit) {
      TheString[Limit] = '\0';
      HasChanged = TRUE;
   } // if

   return HasChanged;
} // BOOLEAN LimitStringLength()

// Takes an input loadpath, splits it into disk and filename components, finds a matching
// DeviceVolume, and returns that and the filename (*loader).
VOID FindVolumeAndFilename(IN EFI_DEVICE_PATH *loadpath, OUT REFIT_VOLUME **DeviceVolume, OUT CHAR16 **loader) {
   CHAR16 *DeviceString, *VolumeDeviceString, *Temp;
   UINTN i = 0;
   BOOLEAN Found = FALSE;

   MyFreePool(*loader);
   MyFreePool(*DeviceVolume);
   *DeviceVolume = NULL;
   DeviceString = DevicePathToStr(loadpath);
   *loader = SplitDeviceString(DeviceString);

   while ((i < VolumesCount) && (!Found)) {
      VolumeDeviceString = DevicePathToStr(Volumes[i]->DevicePath);
      Temp = SplitDeviceString(VolumeDeviceString);
      if (MyStriCmp(DeviceString, VolumeDeviceString)) {
         Found = TRUE;
         *DeviceVolume = Volumes[i];
      }
      MyFreePool(Temp);
      MyFreePool(VolumeDeviceString);
      i++;
   } // while

   MyFreePool(DeviceString);
} // VOID FindVolumeAndFilename()

// Splits a volume/filename string (e.g., "fs0:\EFI\BOOT") into separate
// volume and filename components (e.g., "fs0" and "\EFI\BOOT"), returning
// the filename component in the original *Path variable and the split-off
// volume component in the *VolName variable.
// Returns TRUE if both components are found, FALSE otherwise.
BOOLEAN SplitVolumeAndFilename(IN OUT CHAR16 **Path, OUT CHAR16 **VolName) {
   UINTN i = 0, Length;
   CHAR16 *Filename;

   if (*Path == NULL)
      return FALSE;

   if (*VolName != NULL) {
      MyFreePool(*VolName);
      *VolName = NULL;
   }

   Length = StrLen(*Path);
   while ((i < Length) && ((*Path)[i] != L':')) {
      i++;
   } // while

   if (i < Length) {
      Filename = StrDuplicate((*Path) + i + 1);
      (*Path)[i] = 0;
      *VolName = *Path;
      *Path = Filename;
      return TRUE;
   } else {
      return FALSE;
   }
} // BOOLEAN SplitVolumeAndFilename()

// Returns all the digits in the input string, including intervening
// non-digit characters. For instance, if InString is "foo-3.3.4-7.img",
// this function returns "3.3.4-7". If InString contains no digits,
// the return value is NULL.
CHAR16 *FindNumbers(IN CHAR16 *InString) {
   UINTN i, StartOfElement, EndOfElement = 0, InLength, CopyLength;
   CHAR16 *Found = NULL;

   if (InString == NULL)
      return NULL;

   InLength = StartOfElement = StrLen(InString);
   // Find start & end of target element
   for (i = 0; i < InLength; i++) {
      if ((InString[i] >= '0') && (InString[i] <= '9')) {
         if (StartOfElement > i)
            StartOfElement = i;
         if (EndOfElement < i)
            EndOfElement = i;
      } // if
   } // for
   // Extract the target element
   if (EndOfElement > 0) {
      if (EndOfElement >= StartOfElement) {
         CopyLength = EndOfElement - StartOfElement + 1;
         Found = StrDuplicate(&InString[StartOfElement]);
         if (Found != NULL)
            Found[CopyLength] = 0;
      } // if (EndOfElement >= StartOfElement)
   } // if (EndOfElement > 0)
   return (Found);
} // CHAR16 *FindNumbers()

// Find the #Index element (numbered from 0) in a comma-delimited string
// of elements.
// Returns the found element, or NULL if Index is out of range or InString
// is NULL. Note that the calling function is responsible for freeing the
// memory associated with the returned string pointer.
CHAR16 *FindCommaDelimited(IN CHAR16 *InString, IN UINTN Index) {
   UINTN    StartPos = 0, CurPos = 0;
   BOOLEAN  Found = FALSE;
   CHAR16   *FoundString = NULL;

   if (InString != NULL) {
      // After while() loop, StartPos marks start of item #Index
      while ((Index > 0) && (CurPos < StrLen(InString))) {
         if (InString[CurPos] == L',') {
            Index--;
            StartPos = CurPos + 1;
         } // if
         CurPos++;
      } // while
      // After while() loop, CurPos is one past the end of the element
      while ((CurPos < StrLen(InString)) && (!Found)) {
         if (InString[CurPos] == L',')
            Found = TRUE;
         else
            CurPos++;
      } // while
      if (Index == 0)
         FoundString = StrDuplicate(&InString[StartPos]);
      if (FoundString != NULL)
         FoundString[CurPos - StartPos] = 0;
   } // if
   return (FoundString);
} // CHAR16 *FindCommaDelimited()

// Return the position of SmallString within BigString, or -1 if
// not found.
INTN FindSubString(IN CHAR16 *SmallString, IN CHAR16 *BigString) {
   INTN Position = -1;
   UINTN i = 0, SmallSize, BigSize;
   BOOLEAN Found = FALSE;

   if ((SmallString == NULL) || (BigString == NULL))
      return -1;

   SmallSize = StrLen(SmallString);
   BigSize = StrLen(BigString);
   if ((SmallSize > BigSize) || (SmallSize == 0) || (BigSize == 0))
      return -1;

   while ((i <= (BigSize - SmallSize) && !Found)) {
      if (CompareMem(BigString + i, SmallString, SmallSize) == 0) {
         Found = TRUE;
         Position = i;
      } // if
      i++;
   } // while()
   return Position;
} // INTN FindSubString()

// Take an input path name, which may include a volume specification and/or
// a path, and return separate volume, path, and file names. For instance,
// "BIGVOL:\EFI\ubuntu\grubx64.efi" will return a VolName of "BIGVOL", a Path
// of "EFI\ubuntu", and a Filename of "grubx64.efi". If an element is missing,
// the returned pointer is NULL. The calling function is responsible for
// freeing the allocated memory.
VOID SplitPathName(CHAR16 *InPath, CHAR16 **VolName, CHAR16 **Path, CHAR16 **Filename) {
   CHAR16 *Temp = NULL;

   MyFreePool(*VolName);
   MyFreePool(*Path);
   MyFreePool(*Filename);
   *VolName = *Path = *Filename = NULL;
   Temp = StrDuplicate(InPath);
   SplitVolumeAndFilename(&Temp, VolName); // VolName is NULL or has volume; Temp has rest of path
   CleanUpPathNameSlashes(Temp);
   *Path = FindPath(Temp); // *Path has path (may be 0-length); Temp unchanged.
   *Filename = StrDuplicate(Temp + StrLen(*Path));
   CleanUpPathNameSlashes(*Filename);
   if (StrLen(*Path) == 0) {
      MyFreePool(*Path);
      *Path = NULL;
   }
   if (StrLen(*Filename) == 0) {
      MyFreePool(*Filename);
      *Filename = NULL;
   }
   MyFreePool(Temp);
} // VOID SplitPathName

// Returns TRUE if SmallString is an element in the comma-delimited List,
// FALSE otherwise. Performs comparison case-insensitively.
BOOLEAN IsIn(IN CHAR16 *SmallString, IN CHAR16 *List) {
   UINTN     i = 0;
   BOOLEAN   Found = FALSE;
   CHAR16    *OneElement;

   if (SmallString && List) {
      while (!Found && (OneElement = FindCommaDelimited(List, i++))) {
         if (MyStriCmp(OneElement, SmallString))
            Found = TRUE;
      } // while
   } // if
   return Found;
} // BOOLEAN IsIn()

// Returns TRUE if any element of List can be found as a substring of
// BigString, FALSE otherwise. Performs comparisons case-insensitively.
BOOLEAN IsInSubstring(IN CHAR16 *BigString, IN CHAR16 *List) {
   UINTN   i = 0, ElementLength;
   BOOLEAN Found = FALSE;
   CHAR16  *OneElement;

   if (BigString && List) {
      while (!Found && (OneElement = FindCommaDelimited(List, i++))) {
         ElementLength = StrLen(OneElement);
         if ((ElementLength <= StrLen(BigString)) && (StriSubCmp(OneElement, BigString)))
            Found = TRUE;
      } // while
   } // if
   return Found;
} // BOOLEAN IsSubstringIn()

// Returns TRUE if specified Volume, Directory, and Filename correspond to an
// element in the comma-delimited List, FALSE otherwise. Note that Directory and
// Filename must *NOT* include a volume or path specification (that's part of
// the Volume variable), but the List elements may. Performs comparison
// case-insensitively.
BOOLEAN FilenameIn(REFIT_VOLUME *Volume, CHAR16 *Directory, CHAR16 *Filename, CHAR16 *List) {
   UINTN     i = 0;
   BOOLEAN   Found = FALSE;
   CHAR16    *OneElement;
   CHAR16    *TargetVolName = NULL, *TargetPath = NULL, *TargetFilename = NULL;

   if (Filename && List) {
      while (!Found && (OneElement = FindCommaDelimited(List, i++))) {
         Found = TRUE;
         SplitPathName(OneElement, &TargetVolName, &TargetPath, &TargetFilename);
         VolumeNumberToName(Volume, &TargetVolName);
         if (((TargetVolName != NULL) && ((Volume == NULL) || (!MyStriCmp(TargetVolName, Volume->VolName)))) ||
             ((TargetPath != NULL) && (!MyStriCmp(TargetPath, Directory))) ||
             ((TargetFilename != NULL) && (!MyStriCmp(TargetFilename, Filename)))) {
            Found = FALSE;
         } // if
         MyFreePool(OneElement);
      } // while
   } // if

   MyFreePool(TargetVolName);
   MyFreePool(TargetPath);
   MyFreePool(TargetFilename);
   return Found;
} // BOOLEAN FilenameIn()

// If *VolName is of the form "fs#", where "#" is a number, and if Volume points
// to this volume number, returns with *VolName changed to the volume name, as
// stored in the Volume data structure.
// Returns TRUE if this substitution was made, FALSE otherwise.
BOOLEAN VolumeNumberToName(REFIT_VOLUME *Volume, CHAR16 **VolName) {
   BOOLEAN MadeSubstitution = FALSE;
   UINTN VolNum;

   if ((VolName == NULL) || (*VolName == NULL))
      return FALSE;

   if ((StrLen(*VolName) > 2) && (*VolName[0] == L'f') && (*VolName[1] == L's') && (*VolName[2] >= L'0') && (*VolName[2] <= L'9')) {
      VolNum = Atoi(*VolName + 2);
      if (VolNum == Volume->VolNumber) {
         MyFreePool(*VolName);
         *VolName = StrDuplicate(Volume->VolName);
         MadeSubstitution = TRUE;
      } // if
   } // if
   return MadeSubstitution;
} // BOOLEAN VolumeMatchesNumber()

// Implement FreePool the way it should have been done to begin with, so that
// it doesn't throw an ASSERT message if fed a NULL pointer....
VOID MyFreePool(IN VOID *Pointer) {
   if (Pointer != NULL)
      FreePool(Pointer);
}

static EFI_GUID AppleRemovableMediaGuid = APPLE_REMOVABLE_MEDIA_PROTOCOL_GUID;

// Eject all removable media.
// Returns TRUE if any media were ejected, FALSE otherwise.
BOOLEAN EjectMedia(VOID) {
   EFI_STATUS                      Status;
   UINTN                           HandleIndex, HandleCount = 0, Ejected = 0;
   EFI_HANDLE                      *Handles, Handle;
   APPLE_REMOVABLE_MEDIA_PROTOCOL  *Ejectable;

   Status = LibLocateHandle(ByProtocol, &AppleRemovableMediaGuid, NULL, &HandleCount, &Handles);
   if (EFI_ERROR(Status) || HandleCount == 0)
      return (FALSE); // probably not an Apple system

   for (HandleIndex = 0; HandleIndex < HandleCount; HandleIndex++) {
      Handle = Handles[HandleIndex];
      Status = refit_call3_wrapper(BS->HandleProtocol, Handle, &AppleRemovableMediaGuid, (VOID **) &Ejectable);
      if (EFI_ERROR(Status))
         continue;
      Status = refit_call1_wrapper(Ejectable->Eject, Ejectable);
      if (!EFI_ERROR(Status))
         Ejected++;
   }
   MyFreePool(Handles);
   return (Ejected > 0);
} // VOID EjectMedia()

// Converts consecutive characters in the input string into a
// number, interpreting the string as a hexadecimal number, starting
// at the specified position and continuing for the specified number
// of characters or until the end of the string, whichever is first.
// NumChars must be between 1 and 16. Ignores invalid characters.
UINT64 StrToHex(CHAR16 *Input, UINTN Pos, UINTN NumChars) {
   UINT64 retval = 0x00;
   UINTN  NumDone = 0;
   CHAR16 a;

   if ((Input == NULL) || (StrLen(Input) < Pos) || (NumChars == 0) || (NumChars > 16)) {
      return 0;
   }

   while ((StrLen(Input) >= Pos) && (NumDone < NumChars)) {
      a = Input[Pos];
      if ((a >= '0') && (a <= '9')) {
         retval *= 0x10;
         retval += (a - '0');
         NumDone++;
      }
      if ((a >= 'a') && (a <= 'f')) {
         retval *= 0x10;
         retval += (a - 'a' + 0x0a);
         NumDone++;
      }
      if ((a >= 'A') && (a <= 'F')) {
         retval *= 0x10;
         retval += (a - 'A' + 0x0a);
         NumDone++;
      }
      Pos++;
   } // while()
   return retval;
} // StrToHex()

// Returns TRUE if UnknownString can be interpreted as a GUID, FALSE otherwise.
// Note that the input string must have no extraneous spaces and must be
// conventionally formatted as a 36-character GUID, complete with dashes in
// appropriate places.
BOOLEAN IsGuid(CHAR16 *UnknownString) {
   UINTN   Length, i;
   BOOLEAN retval = TRUE;
   CHAR16  a;

   if (UnknownString == NULL)
      return FALSE;

   Length = StrLen(UnknownString);
   if (Length != 36)
      return FALSE;

   for (i = 0; i < Length; i++) {
      a = UnknownString[i];
      if ((i == 8) || (i == 13) || (i == 18) || (i == 23)) {
         if (a != '-')
            retval = FALSE;
      } else if (((a < 'a') || (a > 'f')) && ((a < 'A') || (a > 'F')) && ((a < '0') && (a > '9'))) {
         retval = FALSE;
      } // if/else if
   } // for
   return retval;
} // BOOLEAN IsGuid()

// Return the GUID as a string, suitable for display to the user. Note that the calling
// function is responsible for freeing the allocated memory.
CHAR16 * GuidAsString(EFI_GUID *GuidData) {
   CHAR16 *TheString;

   TheString = AllocateZeroPool(42 * sizeof(CHAR16));
   if (TheString != 0) {
      SPrint (TheString, 82, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              (UINTN)GuidData->Data1, (UINTN)GuidData->Data2, (UINTN)GuidData->Data3,
              (UINTN)GuidData->Data4[0], (UINTN)GuidData->Data4[1], (UINTN)GuidData->Data4[2],
              (UINTN)GuidData->Data4[3], (UINTN)GuidData->Data4[4], (UINTN)GuidData->Data4[5],
              (UINTN)GuidData->Data4[6], (UINTN)GuidData->Data4[7]);
   }
   return TheString;
} // GuidAsString(EFI_GUID *GuidData)

EFI_GUID StringAsGuid(CHAR16 * InString) {
   EFI_GUID  Guid = NULL_GUID_VALUE;

   if (!IsGuid(InString)) {
      return Guid;
   }

   Guid.Data1 = (UINT32) StrToHex(InString, 0, 8);
   Guid.Data2 = (UINT16) StrToHex(InString, 9, 4);
   Guid.Data3 = (UINT16) StrToHex(InString, 14, 4);
   Guid.Data4[0] = (UINT8) StrToHex(InString, 19, 2);
   Guid.Data4[1] = (UINT8) StrToHex(InString, 21, 2);
   Guid.Data4[2] = (UINT8) StrToHex(InString, 23, 2);
   Guid.Data4[3] = (UINT8) StrToHex(InString, 26, 2);
   Guid.Data4[4] = (UINT8) StrToHex(InString, 28, 2);
   Guid.Data4[5] = (UINT8) StrToHex(InString, 30, 2);
   Guid.Data4[6] = (UINT8) StrToHex(InString, 32, 2);
   Guid.Data4[7] = (UINT8) StrToHex(InString, 34, 2);

   return Guid;
} // EFI_GUID StringAsGuid()

// Returns TRUE if the two GUIDs are equal, FALSE otherwise
BOOLEAN GuidsAreEqual(EFI_GUID *Guid1, EFI_GUID *Guid2) {
   return (CompareMem(Guid1, Guid2, 16) == 0);
} // BOOLEAN GuidsAreEqual()

