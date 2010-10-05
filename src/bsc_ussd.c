/* A USSD Module */
/*
 * (C) 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2010 by On-Waves
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <bsc_data.h>

/*
 * Check the msg and identify a Location Updating Request and see if the
 * LAC is different to this one and then mark the CR message.
 */
int bsc_ussd_handle_out_msg(struct bsc_data *bsc, struct sccp_parse_result *result,
			    struct msgb *msg)
{
	/* Only search for this in the CR message */
	if (sccp_determine_msg_type(msg) != SCCP_MSG_TYPE_CR)
		return 0;

	/* now check the kind of GSM message */

	return 0;
}

/*
 * Check the message if it contains a location update request...
 */
int bsc_ussd_handle_in_msg(struct mtp_link *link, struct sccp_parse_result *res,
			   struct msgb *msg)
{
	return 0;
}
