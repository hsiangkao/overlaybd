/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <gtest/gtest.h>
#include <fcntl.h>
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/subfs.h>
#include <photon/fs/extfs/extfs.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <vector>
#include "../../gzindex/gzfile.h"
#include "../../lsmt/file.h"
#include "../libtar.h"
#include "../tar_file.cpp"
#include "../../gzip/gz.h"
#include "../../../tools/sha256file.h"


#define FILE_SIZE (2 * 1024 * 1024)
#define IMAGE_SIZE 512UL<<20
class TarTest : public ::testing::Test {
protected:
    virtual void SetUp() override{
        fs = photon::fs::new_localfs_adaptor();

        ASSERT_NE(nullptr, fs);
        if (fs->access(workdir.c_str(), 0) != 0) {
            auto ret = fs->mkdir(workdir.c_str(), 0755);
            ASSERT_EQ(0, ret);
        }

        fs = photon::fs::new_subfs(fs, workdir.c_str(), true);
        ASSERT_NE(nullptr, fs);
    }
    virtual void TearDown() override{
        for (auto fn : filelist){
            fs->unlink(fn.c_str());
        }
        if (fs)
            delete fs;
    }

    int download(const std::string &url, std::string out = "") {
        if (out == "") {
            out = workdir + "/" + std::string(basename(url.c_str()));
        }
        if (fs->access(out.c_str(), 0) == 0)
            return 0;
        // download file
        std::string cmd = "curl -s -o " + out + " " + url;
        LOG_INFO(VALUE(cmd.c_str()));
        auto ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
        }
        return 0;
    }

    int download_decomp(const std::string &url) {
        // download file
        std::string cmd = "wget -q -O - " + url +" | gzip -d -c >" +
                          workdir + "/latest.tar";
        LOG_INFO(VALUE(cmd.c_str()));
        auto ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_ERRNO_RETURN(0, -1, "download failed: `", url.c_str());
        }
        return 0;
    }

    int inflate(const char *data, unsigned int size) {
        unsigned char out[65536];
        z_stream strm;
        /* allocate inflate state */
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        ret = inflateInit(&strm);
        if (ret != Z_OK)
           return ret;
        DEFER((void)inflateEnd(&strm));
        strm.avail_in = size;
        strm.next_in = data;
        int fd = open(workdir + "/test.tar".c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
        DEFER(close(fd));
        do {
           strm.avail_out = sizeof(out);
           strm.next_out = out;
           ret = inflate(&strm, Z_NO_FLUSH);
           switch (ret) {
            case Z_NEED_DICT:
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
                return -1;
            }
            int have = sizeof(out) - strm.avail_out;
            if (write(fd, out, have) != have) {
                return -1;
            }
        } while (strm.avail_out == 0);
        return 0;
    }
    int write_file(photon::fs::IFile *file) {
        std::string bb = "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz01";
        ssize_t size = 0;
        ssize_t ret;
        struct stat st;
        LOG_INFO(VALUE(bb.size()));
        while (size < FILE_SIZE) {
            ret = file->write(bb.data(), bb.size());
            EXPECT_EQ(bb.size(), ret);
            ret = file->fstat(&st);
            EXPECT_EQ(0, ret);
            ret = file->lseek(0, SEEK_CUR);
            EXPECT_EQ(st.st_size, ret);
            size += bb.size();
        }
        LOG_INFO("write ` byte", size);
        EXPECT_EQ(FILE_SIZE, size);
        return 0;
    }

    IFile *createDevice(const char *fn, IFile *target_file, size_t virtual_size = IMAGE_SIZE){
        auto fn_idx = std::string(fn)+".idx";
        auto fn_meta = std::string(fn)+".meta";
        DEFER({
            filelist.push_back(fn_idx);
            filelist.push_back(fn_meta);
        });
        auto fmeta = fs->open(fn_idx.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        auto findex = fs->open(fn_meta.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        LSMT::WarpFileArgs args(findex, fmeta, target_file);
        args.virtual_size = virtual_size;
        return create_warpfile(args, false);
    }

    int do_verify(IFile *verify, IFile *test, off_t offset = 0, ssize_t count = -1) {

        if (count == -1) {
            count = verify->lseek(0, SEEK_END);
            auto len = test->lseek(0, SEEK_END);
            if (count != len) {
                LOG_ERROR("check logical length failed");
                return -1;
            }
        }
        LOG_INFO("start verify, virtual size: `", count);

        ssize_t LEN = 1UL<<20;
        char vbuf[1UL<<20], tbuf[1UL<<20];
        // set_log_output_level(0);
        for (off_t i = 0; i < count; i+=LEN) {
            LOG_DEBUG("`", i);
            auto ret_v = verify->pread(vbuf, LEN, i);
            auto ret_t = test->pread(tbuf, LEN, i);
            if (ret_v == -1 || ret_t == -1) {
                LOG_ERROR_RETURN(0, -1, "pread(`,`) failed. (ret_v: `, ret_t: `)",
                    i, LEN, ret_v, ret_v);
            }
            if (ret_v != ret_t) {
                LOG_ERROR_RETURN(0, -1, "compare pread(`,`) return code failed. ret:` / `(expected)",
                    i, LEN, ret_t, ret_v);
            }
            if (memcmp(vbuf, tbuf, ret_v)!= 0){
                LOG_ERROR_RETURN(0, -1, "compare pread(`,`) buffer failed.", i, LEN);
            }
        }
        return 0;
    }

    std::string workdir = "/tmp/tar_test";
    photon::fs::IFileSystem *fs;
    std::vector<std::string> filelist;
};
// photon::fs::IFileSystem *TarTest::fs = nullptr;

TEST_F(TarTest, tar_meta) {
    // set_log_output_level(0);
    ASSERT_EQ(0, download_decomp("https://dadi-shared.oss-cn-beijing.aliyuncs.com/go1.17.6.linux-amd64.tar.gz"));

    auto src_file = fs->open("test.tar", O_RDONLY, 0666);
    ASSERT_NE(nullptr, src_file);
    DEFER(delete src_file);
    auto verify_dev = createDevice("verify", src_file);
    auto tar = new LibErofs(verify_dev, 4096, false);
    ASSERT_EQ(0, tar->extract_tar(src_file, true, true));
    delete tar;

    src_file->lseek(0, 0);

    auto tar_idx = fs->open("test.tar.meta", O_TRUNC | O_CREAT | O_RDWR, 0644);
    auto imgfile = createDevice("mock", src_file);
    DEFER(delete imgfile;);
    auto tar = new UnTar(src_file, nullptr, 0, 4096, nullptr, true);
    auto obj_count = tar->dump_tar_headers(tar_idx);
    EXPECT_NE(-1, obj_count);
    LOG_INFO("objects count: `", obj_count);
    tar_idx->lseek(0,0);

    auto tar = new LibErofs(imagefile, 4096, true);
    ASSERT_EQ(0, tar->extract_tar(src_file, true, true));
    delete tar;
    EXPECT_EQ(0, do_verify(verify_dev, imgfile));
    delete tar_idx;
}

int main(int argc, char **argv) {

    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    set_log_output_level(1);

    auto ret = RUN_ALL_TESTS();
    (void)ret;

    return 0;
}
