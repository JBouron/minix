/* $NetBSD: mech_login.c,v 1.3 2011/02/11 23:44:43 christos Exp $ */

/* Copyright (c) 2010 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Mateusz Kocielski.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.	IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/cdefs.h>
__RCSID("$NetBSD: mech_login.c,v 1.3 2011/02/11 23:44:43 christos Exp $");

#include <assert.h>
#include <saslc.h>
#include <stdio.h>
#include <string.h>

#include "mech.h"
#include "saslc_private.h"


/* Non-standard: no RFC. */

/* properties */
#define SASLC_LOGIN_AUTHCID	SASLC_PROP_AUTHCID
#define SASLC_LOGIN_PASSWD	SASLC_PROP_PASSWD

/**
 * @brief doing one step of the sasl authentication
 * @param sess sasl session
 * @param in input data
 * @param inlen input data length
 * @param out place to store output data
 * @param outlen output data length
 * @return MECH_OK - success,
 * MECH_STEP - more steps are needed,
 * MECH_ERROR - error
 */
/*ARGSUSED*/
static int
saslc__mech_login_cont(saslc_sess_t *sess,  const void *in __unused,
    size_t inlen __unused, void **out, size_t *outlen)
{
	saslc__mech_sess_t *ms = sess->mech_sess;

	switch (ms->step) {
	case 0:
		if (saslc__mech_strdup(sess, (char **)out, outlen,
		    SASLC_LOGIN_AUTHCID,
		    "authcid is required for an authentication") == MECH_OK)
			return MECH_STEP;
		else
			return MECH_ERROR;
	case 1:
		return saslc__mech_strdup(sess, (char **)out, outlen,
		    SASLC_LOGIN_PASSWD,
		    "passwd is required for an authentication");
	default:
		assert(/*CONSTCOND*/0); /* impossible */
		return MECH_ERROR;
	}
}

/* mechanism definition */
const saslc__mech_t saslc__mech_login = {
	.name	 = "LOGIN",
	.flags	 = FLAG_PLAINTEXT,
	.create	 = saslc__mech_generic_create,
	.cont	 = saslc__mech_login_cont,
	.encode	 = NULL,
	.decode	 = NULL,
	.destroy = saslc__mech_generic_destroy
};
