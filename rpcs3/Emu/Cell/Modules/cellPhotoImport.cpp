#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_fs.h"
#include "Emu/RSX/Overlays/overlay_media_list_dialog.h"
#include "Emu/VFS.h"
#include "Emu/System.h"
#include "Utilities/StrUtil.h"
#include "cellSysutil.h"

LOG_CHANNEL(cellPhotoImportUtil, "PhotoImport");

// Return Codes
enum CellPhotoImportError : u32
{
	CELL_PHOTO_IMPORT_ERROR_BUSY         = 0x8002c701,
	CELL_PHOTO_IMPORT_ERROR_INTERNAL     = 0x8002c702,
	CELL_PHOTO_IMPORT_ERROR_PARAM        = 0x8002c703,
	CELL_PHOTO_IMPORT_ERROR_ACCESS_ERROR = 0x8002c704,
	CELL_PHOTO_IMPORT_ERROR_COPY         = 0x8002c705,
	CELL_PHOTO_IMPORT_ERROR_INITIALIZE   = 0x8002c706,
};

template<>
void fmt_class_string<CellPhotoImportError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_PHOTO_IMPORT_ERROR_BUSY);
			STR_CASE(CELL_PHOTO_IMPORT_ERROR_INTERNAL);
			STR_CASE(CELL_PHOTO_IMPORT_ERROR_PARAM);
			STR_CASE(CELL_PHOTO_IMPORT_ERROR_ACCESS_ERROR);
			STR_CASE(CELL_PHOTO_IMPORT_ERROR_COPY);
			STR_CASE(CELL_PHOTO_IMPORT_ERROR_INITIALIZE);
		}

		return unknown;
	});
}

enum CellPhotoImportVersion : u32
{
	CELL_PHOTO_IMPORT_VERSION_CURRENT = 0,
};

enum
{
	CELL_PHOTO_IMPORT_HDD_PATH_MAX           = 1055,
	CELL_PHOTO_IMPORT_PHOTO_TITLE_MAX_LENGTH = 64,
	CELL_PHOTO_IMPORT_GAME_TITLE_MAX_SIZE    = 128,
	CELL_PHOTO_IMPORT_GAME_COMMENT_MAX_SIZE  = 1024
};

enum CellPhotoImportFormatType
{
	CELL_PHOTO_IMPORT_FT_UNKNOWN = 0,
	CELL_PHOTO_IMPORT_FT_JPEG,
	CELL_PHOTO_IMPORT_FT_PNG,
	CELL_PHOTO_IMPORT_FT_GIF,
	CELL_PHOTO_IMPORT_FT_BMP,
	CELL_PHOTO_IMPORT_FT_TIFF,
	CELL_PHOTO_IMPORT_FT_MPO,
};

enum CellPhotoImportTexRot
{
	CELL_PHOTO_IMPORT_TEX_ROT_0 = 0,
	CELL_PHOTO_IMPORT_TEX_ROT_90,
	CELL_PHOTO_IMPORT_TEX_ROT_180,
	CELL_PHOTO_IMPORT_TEX_ROT_270,
};

struct CellPhotoImportFileDataSub
{
	be_t<s32> width;
	be_t<s32> height;
	be_t<CellPhotoImportFormatType> format;
	be_t<CellPhotoImportTexRot> rotate;
};

struct CellPhotoImportFileData
{
	char dstFileName[CELL_FS_MAX_FS_FILE_NAME_LENGTH];
	char photo_title[CELL_PHOTO_IMPORT_PHOTO_TITLE_MAX_LENGTH * 3];
	char game_title[CELL_PHOTO_IMPORT_GAME_TITLE_MAX_SIZE];
	char game_comment[CELL_PHOTO_IMPORT_GAME_COMMENT_MAX_SIZE];
	char padding;
	vm::bptr<CellPhotoImportFileDataSub> data_sub;
	vm::bptr<void> reserved;
};

struct CellPhotoImportSetParam
{
	be_t<u32> fileSizeMax;
	vm::bptr<void> reserved1;
	vm::bptr<void> reserved2;
};

using CellPhotoImportFinishCallback = void(s32 result, vm::ptr<CellPhotoImportFileData> filedata, vm::ptr<void> userdata);

struct photo_import
{
	shared_mutex mutex;
	bool is_busy = false;
	CellPhotoImportSetParam param{};
	vm::ptr<CellPhotoImportFinishCallback> func_finish{};
	vm::ptr<void> userdata{};
};

error_code select_photo(std::string dst_dir)
{
	auto& pi_manager = g_fxo->get<photo_import>();

	if (!pi_manager.func_finish)
		return CELL_PHOTO_IMPORT_ERROR_PARAM;

	if (!fs::is_dir(dst_dir))
	{
		// TODO: check if the dir is user accessible and can be written to
		return CELL_PHOTO_IMPORT_ERROR_PARAM; // TODO: is this correct?
	}

	pi_manager.is_busy = true;

	const std::string vfs_dir_path = vfs::get("/dev_hdd0/photo");
	const std::string title = get_localized_string(localized_string_id::RSX_OVERLAYS_MEDIA_DIALOG_TITLE_PHOTO_IMPORT);

	error_code error = rsx::overlays::show_media_list_dialog(rsx::overlays::media_list_dialog::media_type::photo, vfs_dir_path, title,
		[&pi_manager, dst_dir](s32 status, utils::media_info info)
		{
			sysutil_register_cb([&pi_manager, dst_dir, info, status](ppu_thread& ppu) -> s32
			{
				vm::var<CellPhotoImportFileData> filedata = vm::make_var(CellPhotoImportFileData{});
				vm::var<CellPhotoImportFileDataSub> sub = vm::make_var(CellPhotoImportFileDataSub{});

				u32 result = status >= 0 ? u32{CELL_OK} : u32{CELL_CANCEL};

				if (result == CELL_OK)
				{
					fs::stat_t f_info{};

					if (!fs::stat(info.path, f_info) || f_info.is_directory)
					{
						result = CELL_PHOTO_IMPORT_ERROR_ACCESS_ERROR; // TODO: is this correct ?
						pi_manager.func_finish(ppu, result, filedata, pi_manager.userdata);
						return CELL_OK;
					}

					if (f_info.size > pi_manager.param.fileSizeMax)
					{
						result = CELL_PHOTO_IMPORT_ERROR_COPY; // TODO: is this correct ?
						pi_manager.func_finish(ppu, result, filedata, pi_manager.userdata);
						return CELL_OK;
					}

					const std::string filename = info.path.substr(info.path.find_last_of(fs::delim) + 1);
					const std::string title = info.get_metadata("title", filename);
					const std::string sub_type = fmt::to_lower(info.sub_type);
					const std::string dst_path = dst_dir + "/" + filename;

					strcpy_trunc(filedata->dstFileName, filename);
					strcpy_trunc(filedata->photo_title, title);
					strcpy_trunc(filedata->game_title, Emu.GetTitle());
					// strcpy_trunc(filedata->game_comment, ...); // TODO

					filedata->data_sub = sub;
					filedata->data_sub->width = info.width;
					filedata->data_sub->height = info.height;

					if (sub_type == "jpg" || sub_type == "jpeg")
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_JPEG;
					}
					else if (sub_type == "png")
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_PNG;
					}
					else if (sub_type == "tif" || sub_type == "tiff")
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_TIFF;
					}
					else if (sub_type == "bmp")
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_BMP;
					}
					else if (sub_type == "gif")
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_GIF;
					}
					else if (sub_type == "mpo")
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_MPO;
					}
					else
					{
						filedata->data_sub->format = CELL_PHOTO_IMPORT_FT_UNKNOWN;
					}

					switch (info.orientation)
					{
					default:
					case CELL_SEARCH_ORIENTATION_UNKNOWN:
					case CELL_SEARCH_ORIENTATION_TOP_LEFT:
						filedata->data_sub->rotate = CELL_PHOTO_IMPORT_TEX_ROT_0;
						break;
					case CELL_SEARCH_ORIENTATION_TOP_RIGHT:
						filedata->data_sub->rotate = CELL_PHOTO_IMPORT_TEX_ROT_90;
						break;
					case CELL_SEARCH_ORIENTATION_BOTTOM_RIGHT:
						filedata->data_sub->rotate = CELL_PHOTO_IMPORT_TEX_ROT_180;
						break;
					case CELL_SEARCH_ORIENTATION_BOTTOM_LEFT:
						filedata->data_sub->rotate = CELL_PHOTO_IMPORT_TEX_ROT_270;
						break;
					}

					cellPhotoImportUtil.success("Media list dialog: selected entry '%s'. Copying to '%s'...", info.path, dst_path);
					if (!fs::copy_file(info.path, dst_path, false))
					{
						cellPhotoImportUtil.error("Failed to copy '%s' to '%s'. Error = '%s'", info.path, dst_path, fs::g_tls_error);
						result = CELL_PHOTO_IMPORT_ERROR_COPY;
					}
				}
				else
				{
					cellPhotoImportUtil.notice("Media list dialog was canceled");
				}

				pi_manager.is_busy = false;
				pi_manager.func_finish(ppu, result, filedata, pi_manager.userdata);
				return CELL_OK;
			});
		});

	if (error != CELL_OK)
	{
		pi_manager.is_busy = false;
	}

	return error;
}

error_code cellPhotoImport(u32 version, vm::cptr<char> dstHddPath, vm::ptr<CellPhotoImportSetParam> param, u32 container, vm::ptr<CellPhotoImportFinishCallback> funcFinish, vm::ptr<void> userdata)
{
	cellPhotoImportUtil.todo("cellPhotoImport(version=0x%x, dstHddPath=%s, param=*0x%x, container=0x%x, funcFinish=*0x%x, userdata=*0x%x)", version, dstHddPath, param, container, funcFinish, userdata);

	if (version != CELL_PHOTO_IMPORT_VERSION_CURRENT || !funcFinish || !param || !dstHddPath)
	{
		return CELL_PHOTO_IMPORT_ERROR_PARAM;
	}

	if (container != 0xffffffff && false) // TODO
	{
		return CELL_PHOTO_IMPORT_ERROR_PARAM;
	}

	auto& pi_manager = g_fxo->get<photo_import>();
	std::lock_guard lock(pi_manager.mutex);

	if (pi_manager.is_busy)
	{
		return CELL_PHOTO_IMPORT_ERROR_BUSY;
	}

	pi_manager.param = *param;
	pi_manager.func_finish = funcFinish;
	pi_manager.userdata = userdata;

	return select_photo(dstHddPath.get_ptr());
}

error_code cellPhotoImport2(u32 version, vm::cptr<char> dstHddPath, vm::ptr<CellPhotoImportSetParam> param, vm::ptr<CellPhotoImportFinishCallback> funcFinish, vm::ptr<void> userdata)
{
	cellPhotoImportUtil.todo("cellPhotoImport2(version=0x%x, dstHddPath=%s, param=*0x%x, funcFinish=*0x%x, userdata=*0x%x)", version, dstHddPath, param, funcFinish, userdata);

	if (version != CELL_PHOTO_IMPORT_VERSION_CURRENT || !funcFinish || !param || !dstHddPath)
	{
		return CELL_PHOTO_IMPORT_ERROR_PARAM;
	}

	auto& pi_manager = g_fxo->get<photo_import>();
	std::lock_guard lock(pi_manager.mutex);

	if (pi_manager.is_busy)
	{
		return CELL_PHOTO_IMPORT_ERROR_BUSY;
	}

	pi_manager.param = *param;
	pi_manager.func_finish = funcFinish;
	pi_manager.userdata = userdata;

	return select_photo(dstHddPath.get_ptr());
}

DECLARE(ppu_module_manager::cellPhotoImportUtil)("cellPhotoImportUtil", []()
{
	REG_FUNC(cellPhotoImportUtil, cellPhotoImport);
	REG_FUNC(cellPhotoImportUtil, cellPhotoImport2);
});
