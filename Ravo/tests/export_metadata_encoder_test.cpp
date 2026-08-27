#include "export_metadata_encoder.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::uint16_t read_u16_le(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1U] << 8U));
}

[[nodiscard]] std::uint32_t read_u32_le(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::uint16_t read_u16_be(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1U]);
}

[[nodiscard]] std::string as_text(const std::vector<std::uint8_t> &bytes)
{
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] ExportMetadataSnapshot sample_snapshot()
{
    ExportMetadataSnapshot snapshot;
    snapshot.writable.title = "Title & <quote>";
    snapshot.writable.description = "Desc";
    snapshot.writable.creator = "Alice";
    snapshot.writable.copyright = "© 2026";
    snapshot.capture.camera_make = "RavoCam";
    snapshot.capture.camera_model = "RC-1";
    snapshot.capture.iso = 100.0;
    snapshot.capture.aperture = 2.8;
    snapshot.capture.focal_length_mm = 50.0;
    snapshot.capture.shutter_s = 0.008;
    snapshot.tags = {"alpha", "zeta"};
    return snapshot;
}

TEST(ExportMetadataEncoderTest, PreparesDeterministicOwnedPackets)
{
    const auto snapshot = sample_snapshot();
    const auto first = detail::prepare_export_metadata(snapshot, 640U, 480U, true, {});
    ASSERT_TRUE(first) << first.error().message;
    const auto second = detail::prepare_export_metadata(snapshot, 640U, 480U, true, {});
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value().exif_tiff_profile, second.value().exif_tiff_profile);
    EXPECT_EQ(first.value().xmp_packet, second.value().xmp_packet);
    EXPECT_EQ(first.value().iptc_iim, second.value().iptc_iim);
    EXPECT_EQ(first.value().pixel_width, 640U);
    EXPECT_EQ(first.value().pixel_height, 480U);
    EXPECT_EQ(first.value().orientation, 1U);
    EXPECT_EQ(first.value().color_space, 1U);
    ASSERT_TRUE(first.value().iso);
    EXPECT_EQ(*first.value().iso, 100U);
    ASSERT_TRUE(first.value().aperture);
    EXPECT_EQ(first.value().aperture->numerator, 14U);
    EXPECT_EQ(first.value().aperture->denominator, 5U);
    ASSERT_TRUE(first.value().shutter);
    EXPECT_EQ(first.value().shutter->numerator, 1U);
    EXPECT_EQ(first.value().shutter->denominator, 125U);
    EXPECT_EQ(snapshot, sample_snapshot());
}

TEST(ExportMetadataEncoderTest, ExifTiffProfileIsLittleEndianWithoutJpegPrefix)
{
    const auto prepared = detail::prepare_export_metadata(sample_snapshot(), 12U, 8U, false, {});
    ASSERT_TRUE(prepared) << prepared.error().message;
    const auto &profile = prepared.value().exif_tiff_profile;
    ASSERT_GE(profile.size(), 8U);
    EXPECT_EQ(profile[0], 'I');
    EXPECT_EQ(profile[1], 'I');
    EXPECT_EQ(read_u16_le(profile, 2U), 42U);
    EXPECT_EQ(read_u32_le(profile, 4U), 8U);
    EXPECT_NE(as_text(profile).find(std::string("Exif\0\0", 6U)), 0U);

    const auto ifd0_count = read_u16_le(profile, 8U);
    EXPECT_EQ(ifd0_count, 7U);
    std::uint16_t previous = 0U;
    std::uint32_t exif_offset = 0U;
    for (std::uint16_t index = 0U; index < ifd0_count; ++index)
    {
        const auto field = 10U + (index * 12U);
        const auto tag = read_u16_le(profile, field);
        EXPECT_GT(tag, previous);
        previous = tag;
        if (tag == 274U)
        {
            EXPECT_EQ(read_u16_le(profile, field + 2U), 3U);
            EXPECT_EQ(read_u16_le(profile, field + 8U), 1U);
        }
        if (tag == 34665U)
        {
            exif_offset = read_u32_le(profile, field + 8U);
        }
        if (read_u16_le(profile, field + 2U) == 2U && read_u32_le(profile, field + 4U) > 4U)
        {
            EXPECT_EQ(read_u32_le(profile, field + 8U) % 2U, 0U);
        }
    }
    ASSERT_NE(exif_offset, 0U);
    EXPECT_EQ(exif_offset % 2U, 0U);
    const auto exif_count = read_u16_le(profile, exif_offset);
    EXPECT_EQ(exif_count, 7U);
    bool saw_iso = false;
    bool saw_color = false;
    previous = 0U;
    for (std::uint16_t index = 0U; index < exif_count; ++index)
    {
        const auto field = exif_offset + 2U + (index * 12U);
        const auto tag = read_u16_le(profile, field);
        EXPECT_GT(tag, previous);
        previous = tag;
        if (tag == 34855U)
        {
            saw_iso = true;
            EXPECT_EQ(read_u16_le(profile, field + 8U), 100U);
        }
        if (tag == 40961U)
        {
            saw_color = true;
            EXPECT_EQ(read_u16_le(profile, field + 8U), 0xFFFFU);
        }
        if (tag == 40962U)
        {
            EXPECT_EQ(read_u32_le(profile, field + 8U), 12U);
        }
        if (tag == 40963U)
        {
            EXPECT_EQ(read_u32_le(profile, field + 8U), 8U);
        }
        if (read_u16_le(profile, field + 2U) == 5U)
        {
            EXPECT_EQ(read_u32_le(profile, field + 8U) % 2U, 0U);
        }
    }
    EXPECT_TRUE(saw_iso);
    EXPECT_TRUE(saw_color);
}

TEST(ExportMetadataEncoderTest, XmpUsesFixedNamespacesAndEscapesText)
{
    const auto prepared = detail::prepare_export_metadata(sample_snapshot(), 640U, 480U, true, {});
    ASSERT_TRUE(prepared);
    const auto xml = as_text(prepared.value().xmp_packet);
    EXPECT_EQ(xml.find('\xEF'), std::string::npos);
    EXPECT_NE(xml.find("xmlns:dc=\"http://purl.org/dc/elements/1.1/\""), std::string::npos);
    EXPECT_NE(xml.find("xmlns:exif=\"http://ns.adobe.com/exif/1.0/\""), std::string::npos);
    EXPECT_NE(xml.find("xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\""), std::string::npos);
    EXPECT_NE(xml.find("xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\""), std::string::npos);
    EXPECT_NE(xml.find("<xmp:CreatorTool>Ravo</xmp:CreatorTool>"), std::string::npos);
    EXPECT_NE(xml.find("<tiff:Orientation>1</tiff:Orientation>"), std::string::npos);
    EXPECT_NE(xml.find("<exif:ColorSpace>1</exif:ColorSpace>"), std::string::npos);
    EXPECT_NE(xml.find("<exif:FNumber>14/5</exif:FNumber>"), std::string::npos);
    EXPECT_NE(xml.find("<exif:ExposureTime>1/125</exif:ExposureTime>"), std::string::npos);
    EXPECT_NE(xml.find("<exif:FocalLength>50/1</exif:FocalLength>"), std::string::npos);
    EXPECT_NE(xml.find("Title &amp; &lt;quote&gt;"), std::string::npos);
    EXPECT_NE(xml.find("<rdf:li xml:lang=\"x-default\">Title &amp; &lt;quote&gt;</rdf:li>"),
              std::string::npos);
    EXPECT_LT(xml.find("<dc:title>"), xml.find("<dc:description>"));
    EXPECT_LT(xml.find("<rdf:li>alpha</rdf:li>"), xml.find("<rdf:li>zeta</rdf:li>"));
    EXPECT_EQ(xml.find("DateTimeOriginal"), std::string::npos);
    EXPECT_EQ(xml.find("GPS"), std::string::npos);
}

TEST(ExportMetadataEncoderTest, IptcUsesUtf8RecordVersionAndPresentEmptyDataset)
{
    ExportMetadataSnapshot snapshot;
    snapshot.writable.title = "";
    const auto prepared = detail::prepare_export_metadata(snapshot, 4U, 4U, true, {});
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(prepared.value().iptc_iim);
    const auto &iim = *prepared.value().iptc_iim;
    ASSERT_GE(iim.size(), 20U);
    EXPECT_EQ(iim[0], 0x1CU);
    EXPECT_EQ(iim[1], 1U);
    EXPECT_EQ(iim[2], 90U);
    EXPECT_EQ(read_u16_be(iim, 3U), 3U);
    EXPECT_EQ(iim[5], 0x1B);
    EXPECT_EQ(iim[6], '%');
    EXPECT_EQ(iim[7], 'G');
    EXPECT_EQ(iim[8], 0x1CU);
    EXPECT_EQ(iim[9], 2U);
    EXPECT_EQ(iim[10], 0U);
    EXPECT_EQ(read_u16_be(iim, 11U), 2U);
    EXPECT_EQ(iim[13], 0U);
    EXPECT_EQ(iim[14], 4U);
    EXPECT_EQ(iim[15], 0x1CU);
    EXPECT_EQ(iim[16], 2U);
    EXPECT_EQ(iim[17], 5U);
    EXPECT_EQ(read_u16_be(iim, 18U), 0U);
}

TEST(ExportMetadataEncoderTest, OmitsIptcWhenAllWritablesAndTagsAreAbsent)
{
    const auto prepared =
        detail::prepare_export_metadata(ExportMetadataSnapshot{}, 4U, 4U, true, {});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_FALSE(prepared.value().iptc_iim);
    EXPECT_FALSE(prepared.value().exif_tiff_profile.empty());
    EXPECT_FALSE(prepared.value().xmp_packet.empty());
}

TEST(ExportMetadataEncoderTest, JpegFramingFitsAppMarkerBounds)
{
    const auto prepared = detail::prepare_export_metadata(sample_snapshot(), 640U, 480U, true, {});
    ASSERT_TRUE(prepared);
    const auto exif = detail::build_jpeg_exif_app1_payload(prepared.value().exif_tiff_profile);
    const auto xmp = detail::build_jpeg_xmp_app1_payload(prepared.value().xmp_packet);
    ASSERT_TRUE(exif);
    ASSERT_TRUE(xmp);
    EXPECT_EQ(std::string(exif.value().begin(), exif.value().begin() + 6U),
              std::string("Exif\0\0", 6U));
    EXPECT_LE(exif.value().size(), kJpegAppMarkerMaxPayloadBytes);
    EXPECT_LE(xmp.value().size(), kJpegAppMarkerMaxPayloadBytes);
    ASSERT_TRUE(prepared.value().iptc_iim);
    const auto iptc = detail::build_jpeg_iptc_app13_payload(*prepared.value().iptc_iim);
    ASSERT_TRUE(iptc);
    EXPECT_EQ(std::string(iptc.value().begin(), iptc.value().begin() + 14U),
              std::string("Photoshop 3.0\0", 14U));
    EXPECT_LE(iptc.value().size(), kJpegAppMarkerMaxPayloadBytes);
}

TEST(ExportMetadataEncoderTest, RejectsUnsortedTagsAndInvalidIso)
{
    ExportMetadataSnapshot snapshot;
    snapshot.tags = {"zeta", "alpha"};
    const auto unsorted = detail::prepare_export_metadata(snapshot, 4U, 4U, true, {});
    ASSERT_FALSE(unsorted);
    EXPECT_EQ(unsorted.error().context.at("reason"), "invalid_export_tag_order");

    snapshot.tags = {"alpha"};
    snapshot.capture.iso = 100.5;
    const auto fractional = detail::prepare_export_metadata(snapshot, 4U, 4U, true, {});
    ASSERT_FALSE(fractional);
    EXPECT_EQ(fractional.error().context.at("reason"), "invalid_export_iso_fractional");
}

TEST(ExportMetadataEncoderTest, CancelsBetweenPacketFamilies)
{
    CancellationSource source;
    (void)source.cancel();
    const auto prepared =
        detail::prepare_export_metadata(sample_snapshot(), 8U, 8U, true, source.token());
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().code, ErrorCode::kCancelled);
}

TEST(ExportMetadataEncoderTest, XmlEscapeMatchesDomainEstimator)
{
    constexpr std::string_view text = "A&B<C>\"D'E";
    EXPECT_EQ(detail::xml_escape_utf8(text).size(), xml_escaped_utf8_size(text));
    EXPECT_EQ(detail::xml_escape_utf8(text), "A&amp;B&lt;C&gt;&quot;D&apos;E");
    EXPECT_EQ(detail::xml_escape_utf8("line\rbreak"), "line&#xD;break");
    EXPECT_EQ(detail::xml_escape_utf8("line\rbreak").size(), xml_escaped_utf8_size("line\rbreak"));
}

TEST(ExportMetadataEncoderTest, DomainPacketEstimateIsAConservativePreflight)
{
    const auto snapshot = sample_snapshot();
    const auto sizes = estimate_export_metadata_packets(snapshot);
    ASSERT_TRUE(sizes) << sizes.error().message;
    const auto prepared = detail::prepare_export_metadata(snapshot, 0xFFFFFFFFU, 0xFFFFFFFFU, false,
                                                          CancellationToken{});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_LE(prepared.value().exif_tiff_profile.size(), sizes.value().exif_tiff_profile_bytes);
    EXPECT_LE(prepared.value().xmp_packet.size(), sizes.value().xmp_packet_bytes);
    ASSERT_TRUE(prepared.value().iptc_iim);
    EXPECT_LE(prepared.value().iptc_iim->size(), sizes.value().iptc_iim_bytes);
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
