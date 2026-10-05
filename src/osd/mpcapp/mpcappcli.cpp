// license:BSD-3-Clause
// copyright-holders:MPC3000 for Mac contributors
// Command-line host for the null OSD: the harness runs this binary.

extern "C" int mpc3k_main(int argc, char **argv);

int main(int argc, char **argv)
{
	return mpc3k_main(argc, argv);
}
