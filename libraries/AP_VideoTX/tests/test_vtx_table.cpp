/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// tests for the user-definable VTX band table (AP_VideoTX_Table)

#include <AP_gtest.h>

#include <AP_VideoTX/AP_VideoTX_config.h>

#if AP_VIDEOTX_ENABLED

#include <AP_VideoTX/AP_VideoTX_Table.h>
#include <AP_Math/crc.h>

const AP_HAL::HAL &hal = AP_HAL::get_HAL();

// the historical AP_VideoTX grid, in VideoBand order
static const uint16_t legacy_grid[11][8] = {
    { 5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725 },
    { 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866 },
    { 5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945 },
    { 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880 },
    { 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 },
    { 5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621 },
    { 1080, 1120, 1160, 1200, 1240, 1280, 1320, 1360 },
    { 1080, 1120, 1160, 1200, 1258, 1280, 1320, 1360 },
    { 4990, 5020, 5050, 5080, 5110, 5140, 5170, 5200 },
    { 3330, 3350, 3370, 3390, 3410, 3430, 3450, 3470 },
    { 3170, 3190, 3210, 3230, 3250, 3270, 3290, 3310 },
};

// the defaults reproduce the historical grid exactly, so VTX_BAND indices
// keep their meaning
TEST(VTXTable, DefaultsMatchLegacyGrid)
{
    AP_VideoTX_Table t;
    t.load_defaults();
    ASSERT_EQ(t.num_bands(), 11);
    ASSERT_EQ(t.num_channels(), 8);
    for (uint8_t b = 0; b < 11; b++) {
        for (uint8_t c = 0; c < 8; c++) {
            EXPECT_EQ(t.frequency(b, c), legacy_grid[b][c]) << "band " << int(b) << " ch " << int(c);
        }
    }
    EXPECT_EQ(t.band_letter(4), 'R');
    EXPECT_TRUE(t.is_valid());
}

TEST(VTXTable, OutOfRangeLookups)
{
    AP_VideoTX_Table t;
    t.load_defaults();
    EXPECT_EQ(t.frequency(11, 0), 0);
    EXPECT_EQ(t.frequency(0, 8), 0);
    EXPECT_EQ(t.band_letter(11), '?');
    uint8_t band, channel;
    EXPECT_FALSE(t.band_and_channel_for_frequency(0, band, channel));
    EXPECT_FALSE(t.band_and_channel_for_frequency(5999, band, channel));
}

// reverse lookup returns the first match, so a frequency shared by two bands
// (1080 in 1G3_A and 1G3_B) resolves to the earlier band as before
TEST(VTXTable, ReverseLookupFirstMatch)
{
    AP_VideoTX_Table t;
    t.load_defaults();
    uint8_t band = 0xFF, channel = 0xFF;
    ASSERT_TRUE(t.band_and_channel_for_frequency(5806, band, channel));
    EXPECT_EQ(band, 4);
    EXPECT_EQ(channel, 4);
    ASSERT_TRUE(t.band_and_channel_for_frequency(1080, band, channel));
    EXPECT_EQ(band, 6);
    EXPECT_EQ(channel, 0);
}

#if AP_VIDEOTX_TABLE_ENABLED

// recompute the trailing CRC after editing a blob body
static void fix_crc(uint8_t *blob, uint16_t len)
{
    const uint32_t crc = crc_crc32(0, blob, len - 4);
    blob[len-4] = crc & 0xFF;
    blob[len-3] = (crc >> 8) & 0xFF;
    blob[len-2] = (crc >> 16) & 0xFF;
    blob[len-1] = (crc >> 24) & 0xFF;
}

TEST(VTXTable, SerializedDefaultsValidate)
{
    AP_VideoTX_Table t;
    t.load_defaults();
    uint8_t blob[AP_VideoTX_Table::BLOB_MAX];
    const uint16_t len = t.to_blob(blob);
    EXPECT_LE(len, uint16_t(AP_VideoTX_Table::BLOB_MAX));
    EXPECT_EQ(len, uint16_t(AP_VideoTX_Table::BLOB_HEADER + 11*(AP_VideoTX_Table::BAND_NAME_LEN+2+8*2) + 4));
    EXPECT_EQ(blob[2], uint8_t(AP_VideoTX_Table::BLOB_VERSION));
    EXPECT_TRUE(AP_VideoTX_Table::validate(blob, len));
}

TEST(VTXTable, ValidateRejectsCorruption)
{
    AP_VideoTX_Table t;
    t.load_defaults();
    uint8_t blob[AP_VideoTX_Table::BLOB_MAX];
    const uint16_t len = t.to_blob(blob);

    uint8_t bad[AP_VideoTX_Table::BLOB_MAX];

    // flipped payload byte: CRC mismatch
    memcpy(bad, blob, len);
    bad[20] ^= 0x01;
    EXPECT_FALSE(AP_VideoTX_Table::validate(bad, len));

    // truncated
    EXPECT_FALSE(AP_VideoTX_Table::validate(blob, len - 1));
    EXPECT_FALSE(AP_VideoTX_Table::validate(blob, 4));

    // wrong magic / version, even with a matching CRC
    memcpy(bad, blob, len);
    bad[0] ^= 0xFF;
    fix_crc(bad, len);
    EXPECT_FALSE(AP_VideoTX_Table::validate(bad, len));
    memcpy(bad, blob, len);
    bad[2] = 1;
    fix_crc(bad, len);
    EXPECT_FALSE(AP_VideoTX_Table::validate(bad, len));
}

TEST(VTXTable, ValidateRejectsBadDimensions)
{
    uint8_t blob[AP_VideoTX_Table::BLOB_MAX + 64] {};
    blob[0] = AP_VideoTX_Table::BLOB_MAGIC & 0xFF;
    blob[1] = AP_VideoTX_Table::BLOB_MAGIC >> 8;
    blob[2] = AP_VideoTX_Table::BLOB_VERSION;

    // empty tables: would resolve every band/channel to 0 MHz
    blob[3] = 0; blob[4] = 8;
    fix_crc(blob, AP_VideoTX_Table::BLOB_HEADER + 4);
    EXPECT_FALSE(AP_VideoTX_Table::validate(blob, AP_VideoTX_Table::BLOB_HEADER + 4));
    blob[3] = 1; blob[4] = 0;
    fix_crc(blob, AP_VideoTX_Table::BLOB_HEADER + 10 + 4);
    EXPECT_FALSE(AP_VideoTX_Table::validate(blob, AP_VideoTX_Table::BLOB_HEADER + 10 + 4));

    // over the limits
    blob[3] = AP_VideoTX_Table::MAX_BANDS + 1; blob[4] = 8;
    EXPECT_FALSE(AP_VideoTX_Table::validate(blob, sizeof(blob)));
    blob[3] = 1; blob[4] = AP_VideoTX_Table::MAX_CHANNELS + 1;
    EXPECT_FALSE(AP_VideoTX_Table::validate(blob, sizeof(blob)));
}

// a table with fewer bands/channels than the defaults is valid, and trailing
// bytes beyond the declared size are ignored
TEST(VTXTable, ValidateAcceptsSmallTable)
{
    const uint16_t len = AP_VideoTX_Table::BLOB_HEADER + 1*(AP_VideoTX_Table::BAND_NAME_LEN+2+4*2) + 4;
    uint8_t blob[64] {};
    blob[0] = AP_VideoTX_Table::BLOB_MAGIC & 0xFF;
    blob[1] = AP_VideoTX_Table::BLOB_MAGIC >> 8;
    blob[2] = AP_VideoTX_Table::BLOB_VERSION;
    blob[3] = 1;
    blob[4] = 4;
    memcpy(&blob[5], "CUSTOM", 6);
    blob[13] = 'Z';
    fix_crc(blob, len);
    EXPECT_TRUE(AP_VideoTX_Table::validate(blob, len));
    EXPECT_TRUE(AP_VideoTX_Table::validate(blob, sizeof(blob)));
}

// SITL emulates 16k flash storage, which has no table region: an upload must
// be refused without touching the table
TEST(VTXTable, UploadRefusedWithoutStorage)
{
    ASSERT_FALSE(AP_VideoTX_Table::storage_available());
    AP_VideoTX_Table t;
    t.load_defaults();
    uint8_t blob[AP_VideoTX_Table::BLOB_MAX];
    const uint16_t len = t.to_blob(blob);
    blob[AP_VideoTX_Table::BLOB_HEADER + 10] = 0x6F;  // band0 ch0 -> 5999
    blob[AP_VideoTX_Table::BLOB_HEADER + 11] = 0x17;
    fix_crc(blob, len);
    ASSERT_TRUE(AP_VideoTX_Table::validate(blob, len));
    EXPECT_FALSE(t.from_blob(blob, len));
    EXPECT_EQ(t.frequency(0, 0), 5865);
}

#endif  // AP_VIDEOTX_TABLE_ENABLED

AP_GTEST_MAIN()

#endif  // AP_VIDEOTX_ENABLED
