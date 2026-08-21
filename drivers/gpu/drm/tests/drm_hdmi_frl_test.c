// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the HDMI 2.1 FRL helpers
 */

#include <linux/hdmi.h>

#include <kunit/test.h>

/*
 * The FRL configurations defined by the HDMI 2.1+ specification, and the
 * total bandwidth each of them provides.
 */
static const struct {
	u8 rate_per_lane;
	u8 lanes;
	u32 gbps;
} hdmi_frl_valid_configs[] = {
	{  3, 3,  9 },
	{  6, 3, 18 },
	{  6, 4, 24 },
	{  8, 4, 32 },
	{ 10, 4, 40 },
	{ 12, 4, 48 },
	{ 16, 4, 64 },
	{ 20, 4, 80 },
	{ 24, 4, 96 },
};

/*
 * Test that every rate/lane combination the specification defines is
 * recognised, and that its bandwidth decomposes back into it.
 */
static void hdmi_test_frl_config_roundtrip(struct kunit *test)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hdmi_frl_valid_configs); i++) {
		u8 expected_rate = hdmi_frl_valid_configs[i].rate_per_lane;
		u8 expected_lanes = hdmi_frl_valid_configs[i].lanes;
		u32 gbps = hdmi_frl_valid_configs[i].gbps;
		u8 rate_per_lane, lanes;
		int ret;

		KUNIT_EXPECT_TRUE_MSG(test,
				      hdmi_is_valid_frl_config(expected_rate, expected_lanes),
				      "%u Gbps x %u lanes rejected",
				      expected_rate, expected_lanes);

		ret = hdmi_frl_config_from_bandwidth(gbps, &rate_per_lane, &lanes);
		KUNIT_EXPECT_EQ_MSG(test, ret, 0, "%u Gbps not decomposed", gbps);
		KUNIT_EXPECT_EQ(test, rate_per_lane, expected_rate);
		KUNIT_EXPECT_EQ(test, lanes, expected_lanes);
	}
}

struct hdmi_frl_config_case {
	const char *desc;
	u8 rate_per_lane;
	u8 lanes;
};

/*
 * Test that rate/lane combinations outside the ones defined by the
 * specification are rejected, even when both values are individually within
 * the allowed range.
 */
static void hdmi_test_is_valid_frl_config_invalid(struct kunit *test)
{
	const struct hdmi_frl_config_case *params = test->param_value;

	KUNIT_EXPECT_FALSE(test, hdmi_is_valid_frl_config(params->rate_per_lane,
							 params->lanes));
}

static const struct hdmi_frl_config_case hdmi_frl_config_invalid_tests[] = {
	{ "zero",		 0,  0 },
	{ "no-lanes",		 6,  0 },
	{ "no-rate",		 0,  4 },
	{ "3gbps-4lanes",	 3,  4 },
	{ "8gbps-3lanes",	 8,  3 },
	{ "12gbps-3lanes",	12,  3 },
	{ "5gbps-4lanes",	 5,  4 },
	{ "24gbps-3lanes",	24,  3 },
	{ "above-max-rate",	32,  4 },
	{ "above-max-lanes",	 6,  5 },
};

static void hdmi_frl_config_desc(const struct hdmi_frl_config_case *t, char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(hdmi_frl_config_invalid, hdmi_frl_config_invalid_tests,
		  hdmi_frl_config_desc);

struct hdmi_frl_bandwidth_case {
	const char *desc;
	u32 gbps;
};

/*
 * Test that a total bandwidth not matching any of the configurations defined
 * by the specification is rejected, and the outputs are left untouched.
 */
static void hdmi_test_frl_config_from_bandwidth_invalid(struct kunit *test)
{
	const struct hdmi_frl_bandwidth_case *params = test->param_value;
	u8 rate_per_lane = 0xff, lanes = 0xff;
	int ret;

	ret = hdmi_frl_config_from_bandwidth(params->gbps, &rate_per_lane, &lanes);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, rate_per_lane, 0xff);
	KUNIT_EXPECT_EQ(test, lanes, 0xff);
}

static const struct hdmi_frl_bandwidth_case hdmi_frl_bandwidth_invalid_tests[] = {
	{ "zero",		0 },
	{ "below-min",		8 },
	{ "between-9-18",	12 },
	{ "between-24-32",	25 },
	{ "between-48-64",	56 },
	{ "above-max",		100 },
};

static void hdmi_frl_bandwidth_desc(const struct hdmi_frl_bandwidth_case *t, char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(hdmi_frl_bandwidth_invalid, hdmi_frl_bandwidth_invalid_tests,
		  hdmi_frl_bandwidth_desc);

struct hdmi_frl_range_case {
	const char *desc;
	unsigned long long clock;
	unsigned int source_min_gbps;
	unsigned int source_max_gbps;
	unsigned int sink_max_gbps;
	int expected_ret;
	unsigned int expected_min_gbps;
	unsigned int expected_max_gbps;
};

/*
 * Test that the FRL bandwidth range required to carry a mode is the lowest
 * sufficient configuration paired with the highest one the source and the
 * sink both allow, and that an unusable capability window is reported as
 * such.
 */
static void hdmi_test_frl_bandwidth_range_from_clock(struct kunit *test)
{
	const struct hdmi_frl_range_case *params = test->param_value;
	unsigned int min_gbps = 0, max_gbps = 0;
	int ret;

	ret = hdmi_frl_bandwidth_range_from_clock(params->clock,
						  params->source_min_gbps,
						  params->source_max_gbps,
						  params->sink_max_gbps,
						  &min_gbps, &max_gbps);
	KUNIT_EXPECT_EQ(test, ret, params->expected_ret);

	if (params->expected_ret)
		return;

	KUNIT_EXPECT_EQ(test, min_gbps, params->expected_min_gbps);
	KUNIT_EXPECT_EQ(test, max_gbps, params->expected_max_gbps);
	KUNIT_EXPECT_LE(test, min_gbps, max_gbps);
}

/*
 * 4K@60Hz has a 594 MHz pixel clock, which yields a 594 MHz TMDS character
 * rate at 8 bpc and 891 MHz at 12 bpc.  The FRL bandwidth needed to carry
 * those is 16.038 Gbps and 24.057 Gbps respectively.
 */
static const struct hdmi_frl_range_case hdmi_frl_range_tests[] = {
	{ "4k60-8bpc",		594000000,  9, 48, 48, 0, 18, 48 },
	{ "4k60-12bpc",		891000000,  9, 48, 48, 0, 32, 48 },
	{ "sink-capped",	594000000,  9, 48, 24, 0, 18, 24 },
	{ "source-capped",	594000000,  9, 24, 48, 0, 18, 24 },
	{ "source-min-raised",	594000000, 24, 48, 48, 0, 24, 48 },
	{ "exact-fit",		666666666,  9, 48, 48, 0, 18, 48 },
	{ "just-above-fit",	666666667,  9, 48, 48, 0, 24, 48 },
	{ "window-empty",	594000000, 64, 96, 48, -EINVAL },
	{ "mode-too-demanding",	891000000,  9, 24, 24, -EINVAL },
	{ "beyond-max-config",	4000000000, 9, 96, 96, -EINVAL },
};

static void hdmi_frl_range_desc(const struct hdmi_frl_range_case *t, char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(hdmi_frl_range, hdmi_frl_range_tests, hdmi_frl_range_desc);

static struct kunit_case hdmi_frl_tests[] = {
	KUNIT_CASE(hdmi_test_frl_config_roundtrip),
	KUNIT_CASE_PARAM(hdmi_test_is_valid_frl_config_invalid,
			 hdmi_frl_config_invalid_gen_params),
	KUNIT_CASE_PARAM(hdmi_test_frl_config_from_bandwidth_invalid,
			 hdmi_frl_bandwidth_invalid_gen_params),
	KUNIT_CASE_PARAM(hdmi_test_frl_bandwidth_range_from_clock,
			 hdmi_frl_range_gen_params),
	{ }
};

static struct kunit_suite hdmi_frl_test_suite = {
	.name		= "hdmi_frl",
	.test_cases	= hdmi_frl_tests,
};

kunit_test_suite(hdmi_frl_test_suite);

MODULE_AUTHOR("Cristian Ciocaltea <cristian.ciocaltea@collabora.com>");
MODULE_DESCRIPTION("Kunit test for the HDMI 2.1 FRL helpers");
MODULE_LICENSE("GPL");
