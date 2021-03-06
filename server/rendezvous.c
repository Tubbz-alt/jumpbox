#include "djb.h"

#include "defiantclient.h"
#include "defiantbf.h"
#include "defiant_params.h"
#include "defiantrequest.h"
#include "defianterrors.h"

#include "onion.h"
#include "jpeg_steg.h"

#include <openssl/sha.h>

#define KBYTE 1024

/* Show progress */
#undef RDV_VERBOSE

static char l_password[DEFIANT_REQ_REP_PASSWORD_LENGTH + 1];

/* we are currently forcing passwords to start with "aaa" */
static const long maxAttempts = 1 * 1 * 1 * 26 * 26 * 26 * 26 * 26;

/* Variables we currently work with */
static onion_t		l_current_onion = NULL;
/* reset will unlink these */
static const char	*l_current_image_path = NULL;
static const char	*l_current_image_dir = NULL;
/* captcha goes in the same directory */
static const char	*l_captcha_image_path = NULL;

/* pow thread */
static int		l_pow_thread_started = 0;
static int		l_pow_thread_finished = 0;
static int		l_pow_thread_quit = 0;
static onion_t		l_pow_inner_onion = NULL;
static volatile long	l_pow_thread_progress = 0;

static const char *
rdv_randompath(void);
static const char *
rdv_randompath(void) {
	uint64_t	r = generate_random_number();

	/* Looks like flickr for now */
	return (aprintf("photos/26907150@N08/%" PRIu64 "/lightbox", r));
}

static void
rdv_onion_reset(void);
static void
rdv_onion_reset(void) {
	log_dbg("...");

	memset(l_password, 0, DEFIANT_REQ_REP_PASSWORD_LENGTH + 1);

	if (l_current_onion != NULL) {
		free_onion(l_current_onion);
		l_current_onion = NULL;
	}
}

static void
rdv_pow_reset(void);
static void
rdv_pow_reset(void) {
	log_dbg("...");

	if (l_pow_thread_started) {
		onion_t inner;

		l_pow_thread_quit = 1;
		inner = l_pow_inner_onion;

		if (inner != NULL) {
			l_pow_inner_onion = NULL;
			free_onion(inner);
		}

		l_pow_thread_started = 0;
		l_pow_thread_finished = 0;
		l_pow_thread_progress = 0;
	}
}

static void
rdv_captcha_reset(void);
static void
rdv_captcha_reset(void) {
	log_dbg("...");

	if (l_captcha_image_path != NULL) {
		if (unlink(l_captcha_image_path) == -1) {
			log_dbg("unlink(%s) failed: %s",
				l_captcha_image_path, strerror(errno));
		}

		aprintf_free(l_captcha_image_path);
		l_captcha_image_path = NULL;
	}
}

static void
rdv_image_reset(void);
static void
rdv_image_reset(void) {
	log_dbg("...");

	if (l_current_image_path != NULL) {
		if (unlink(l_current_image_path) == -1) {
			log_wrn("unlink(%s) failed: %s",
				l_current_image_path, strerror(errno));
		};

		if (rmdir(l_current_image_dir) == -1) {
			log_wrn("rmdir(%s) failed: %s",
				l_current_image_dir, strerror(errno));
		};

		free((void *)l_current_image_path);
		free((void *)l_current_image_dir);
		l_current_image_path = NULL;
		l_current_image_dir = NULL;
	}
}

static void
rdv_reset(httpsrv_client_t *hcl);
static void
rdv_reset(httpsrv_client_t *hcl) {
	rdv_onion_reset();
	rdv_captcha_reset();
	rdv_image_reset();
	rdv_pow_reset();

	djb_presult(hcl, "Reset OK");
}

static void
rdv_gen_request_aux(httpsrv_client_t *hcl, const char *server, bool secure);
static void
rdv_gen_request_aux(httpsrv_client_t *hcl, const char *server, bool secure) {
	const char	*path = NULL;
	char		*request = NULL;
	bf_params_t	*params =  NULL;

	int defcode = bf_char64_to_params(defiant_params_P, defiant_params_Ppub, &params);
	if (defcode == DEFIANT_OK) {
		randomPasswordEx(l_password,
				 DEFIANT_REQ_REP_PASSWORD_LENGTH + 1, 0);
		path = rdv_randompath();

		if (secure) {
			defcode = generate_defiant_ssl_request_url(
				  params, l_password, server, path, &request);
		} else {
			defcode = generate_defiant_request_url(
				  params, l_password, server, path, &request);
		}
	}

	if (defcode == DEFIANT_OK) {
		djb_presult(hcl, request);

		log_dbg("secure=%s, password=%s, request=%s",
			yesno(secure),
			l_password, request);
	} else {
		djb_error(hcl, 400, defiant_strerror(defcode));
	}

	bf_free_params(params);

	if (path != NULL) {
		aprintf_free(path);
	}

	if (request != NULL) {
		free(request);
	}
}

static void
rdv_gen_request(httpsrv_client_t *hcl);
static void
rdv_gen_request(httpsrv_client_t *hcl) {
	json_error_t	error;
	json_t		*root;
	const char	*server = NULL;
	bool		secure = false;

	if (hcl->method != HTTP_M_POST) {
		djb_error(hcl, 400, "gen_request requires a POST");
		return;
	}

	/* No body yet? Then allocate some memory to get it */
	if (hcl->readbody == NULL) {
		if (hcl->headers.content_length == 0) {
			djb_error(hcl, 400, "gen_request requires length");
			return;
		}

		if (httpsrv_readbody_alloc(hcl, 2, 0) < 0) {
			log_dbg("httpsrv_readbody_alloc() failed");
		}

		return;
	}

	log_dbg("data: %s", hcl->readbody);

	root = json_loads(hcl->readbody, 0, &error);
	httpsrv_readbody_free(hcl);

	if (root == NULL) {
		log_dbg("JSON load failed");
		return;

	} else if (json_is_object(root)) {
		json_t *server_val, *secure_val;

		secure_val = json_object_get(root, "secure");
		if (secure_val != NULL && json_is_true(secure_val)) {
			secure = true;
		}

		server_val = json_object_get(root, "server");

		if (server_val != NULL && json_is_string(server_val)) {
			server = json_string_value(server_val);
		}
	}

	if (server != NULL) {
		rdv_gen_request_aux(hcl, server, secure);
	} else {
		djb_error(hcl, 400, "POST data conundrum");

		if (root == NULL) {
			log_dbg("data: %s, error: line: %u, msg: %s",
				hcl->readbody, error.line, error.text);
		}
	}

	json_decref(root);
}

static const char onion_names[5][10] = {
	"base",
	"pow",
	"captcha",
	"signed",
	"collection"
};

static const char *
rdv_onion_name(enum onion_type onion_type);
static const char *
rdv_onion_name(enum onion_type onion_type) {
	fassert(onion_type < lengthof(onion_names));

	return (onion_names[onion_type]);
}

static const char *
rdv_make_image_response(const char *path, int onion_type);
static const char *
rdv_make_image_response(const char *path, int onion_type) {
	return (aprintf("{ \"image\": \"/rendezvous/file%s\", "
			  "\"onion_type\": \"%s\"}",
			path, rdv_onion_name(onion_type)));
}

static void
rdv_image(httpsrv_client_t *hcl);
static void
rdv_image(httpsrv_client_t *hcl) {
	const char	*response = NULL;
	char		*image_path = NULL,
			*image_dir = NULL,
			*encrypted_onion = NULL,
			*onion = NULL;
	size_t		encrypted_onion_sz = 0;
	int		retcode = DEFIANT_OK;

	if (hcl->method != HTTP_M_POST) {
		djb_error(hcl, 400, "gen_request requires a POST");
		return;
	}

	log_dbg("readbody: %s", yesno(hcl->readbody == NULL));

	/* No body yet? Then allocate some memory to get it */
	if (hcl->readbody == NULL) {
		if (hcl->headers.content_length == 0) {
			djb_error(hcl, 400, "image requires length");
			return;
		}

		if (httpsrv_readbody_alloc(hcl, 0, 0) < 0) {
			log_dbg("httpsrv_readbody_alloc() failed");
		}

		return;
	}

	retcode = extract_n_save(l_password, hcl->readbody, hcl->readbody_off,
				 &encrypted_onion, &encrypted_onion_sz,
				 &image_path, &image_dir);

	httpsrv_readbody_free(hcl);

	if (retcode != DEFIANT_OK) {
		log_dbg("extract_n_save() with password=%s returned %d -- %s",
			l_password, retcode, defiant_strerror(retcode));

		djb_error(hcl, 400,
			 "extract_n_save() failure, see log)");

	} else {
		int onion_sz = 0;
		onion = (onion_t)defiant_pwd_decrypt(l_password,
				(const uchar *)encrypted_onion,
				encrypted_onion_sz, &onion_sz);

		if (onion == NULL) {
			log_dbg("Decrypting onion failed: No onion");

		} else if (onion_sz < (int)sizeof(onion_header_t)) {
			log_dbg("Decrypting onion failed: onion_sz less "
				"than onion header");

		} else if (!ONION_IS_ONION(onion)) {
			log_dbg("Decrypting onion failed: Onion Magic Incorrect");

		} else if (onion_sz != (int)ONION_SIZE(onion)) {
			log_dbg("Decrypting onion failed: onion_sz (%u) "
				"does nat match real onion sizeu(%d)",
				onion_sz, (int)ONION_SIZE(onion));
		} else {
			response = rdv_make_image_response(image_path,
				    ONION_TYPE(onion));

			if (response != NULL) {
				log_dbg("response %s", response);

				l_current_onion = (onion_t)onion;
				l_current_image_path = image_path;
				l_current_image_dir = image_dir;

				log_dbg("onion_sz %u, "
					"onion_type: %s",
					onion_sz,
					rdv_onion_name(
					  ONION_TYPE(l_current_onion)));

				djb_presult(hcl, response);
			}
		}
	}

	if (response == NULL) {
		/* these need to be reclaimed when things go wrong */
		if (image_path != NULL) {
			unlink(image_path);
			unlink(image_dir);
			free(image_path);
			free(image_dir);
		}

		free(onion);
		djb_error(hcl, 400, "server error");
	} else {
		aprintf_free(response);
	}

	/* rain or shine these can get tossed */
	if (encrypted_onion != NULL) {
		free(encrypted_onion);
	}
}

static const char *
rdv_make_peel_response(const char *info, const char *status);
static const char *
rdv_make_peel_response(const char *info, const char *status) {
	unsigned int onion_type = ONION_TYPE(l_current_onion);

	return (aprintf("{ \"info\": \"%s\", "
			  "\"status\": \"%s\", "
			  "\"onion_type\": \"%s\"}",
			info, status, rdv_onion_name(onion_type)));
}

static const char *
rdv_make_pow_response(unsigned int percent, const char *status);
static const char *
rdv_make_pow_response(unsigned int percent, const char *status) {
	unsigned int onion_type = ONION_TYPE(l_current_onion);

	return (aprintf("{ \"info\": %u, "
			  "\"status\": \"%s\", "
			  "\"onion_type\": \"%s\"}",
			percent, status, rdv_onion_name(onion_type)));
}

static const char *
rdv_peel_base(void);
static const char *
rdv_peel_base(void) {
	const char	*response = NULL;
	const char	*nep;
	json_error_t	error;
	json_t		*root;

	/* Just in case */
	fassert(ONION_TYPE(l_current_onion) == BASE);

	nep = (char *)ONION_DATA(l_current_onion);

	log_dbg("nep = %s", nep);

	root = json_loads(nep, 0, &error);
	if (root != NULL) {
		/* Pass the NET to ACS so that it can Dance */
		acs_set_net(root);
		/* XXX: might fail if already dancing, check return */

		response = rdv_make_peel_response(
			"NET passed to ACS", "Complete");

		/* Done with it here */
		json_decref(root);
	} else {
		log_dbg("data = %s error: line: %d msg: %s",
			 nep, error.line, error.text);
		response = rdv_make_peel_response(
			"Sorry your nep did not parse as JSON", "");
	}

	return (response);
}

static void *
rdv_pow_worker(void UNUSED *arg);
static void *
rdv_pow_worker(void UNUSED *arg) {
	size_t	puzzle_size = ONION_PUZZLE_SIZE(l_current_onion);
	char	*hash = ONION_PUZZLE(l_current_onion);
	int	hash_len = SHA_DIGEST_LENGTH;
	char	*secret = hash + SHA_DIGEST_LENGTH;
	int	secret_len = puzzle_size - SHA_DIGEST_LENGTH;
	char	*data = ONION_DATA(l_current_onion);
	size_t	data_len = ONION_DATA_SIZE(l_current_onion);

	l_pow_inner_onion = defiant_pow_aux((uchar *)hash, hash_len,
					   (uchar *)secret, secret_len,
					   (uchar *)data, data_len,
					   &l_pow_thread_progress);

	log_dbg("pow_inner_onion = %p : %s %s",
		l_pow_inner_onion, (char *)l_pow_inner_onion,
		(char *)ONION_DATA(l_pow_inner_onion));

	l_pow_thread_progress = maxAttempts;
	l_pow_thread_finished = 1;

	return (NULL);
}


static unsigned int
rdv_attempts2percent(void);
static unsigned int
rdv_attempts2percent(void) {
	unsigned int	retval = 0;
	unsigned long	current = l_pow_thread_progress;

	retval = ((current * 100) / maxAttempts);

#ifdef RDV_VERBOSE
	log_dbg("%lu %u%%\n", current, retval);
#endif
	return (retval);
}

static const char *
rdv_peel_pow(void);
static const char *
rdv_peel_pow(void) {
	const char	*response = NULL;
	unsigned int	pc = rdv_attempts2percent();

	if (l_pow_thread_started == 0) {
		/* Start the POW thread */
		if (thread_add("RendezvousPOW", rdv_pow_worker, NULL)) {
			l_pow_thread_started = 1;
			response = rdv_make_pow_response(pc,
				"OK the Proof-Of-Work has commenced");
		} else {
			response = rdv_make_peel_response("",
				"Creating the Proof-Of-Work failed :-(");
		}

	} else {
		/*
		 * Monitor the progress of the thread;
		 * or do the current <--> inner switch
		 */
		if (l_pow_thread_finished == 0) {
			response = rdv_make_pow_response(pc,
				   "Working away...");
		} else {
			if (l_pow_inner_onion == NULL) {
				response = rdv_make_peel_response("",
					"Proof of work FAILED?!?");
			} else {
				response = rdv_make_pow_response(100,
					"Your Proof-Of-Work has "
					"finished successfully!");

				/* Free the old onion */
				free_onion(l_current_onion);

				/* This is the new one */
				l_current_onion = l_pow_inner_onion;

				l_pow_inner_onion = NULL;
				rdv_pow_reset();
			}
		}
	}

	return (response);
}

static const char *
rdv_peel_captcha_no_image_path(void);
static const char *
rdv_peel_captcha_no_image_path(void) {
	int		retcode;
	const char	*path, *ret;

	fassert(l_current_image_dir != NULL);

	l_captcha_image_path =
		aprintf("%s" PATH_SEPARATOR "captcha.png",
			l_current_image_dir);
	if (l_captcha_image_path == NULL) {
		return (NULL);
	}

	log_dbg("Captcha image = %s",
		l_captcha_image_path);

	retcode = bytes2file(l_captcha_image_path,
			     ONION_PUZZLE_SIZE(l_current_onion),
			     ONION_PUZZLE(l_current_onion));

	if (retcode != DEFIANT_OK) {
		log_dbg("Could not store captcha to %s: %s",
			l_captcha_image_path,
			defiant_strerror(retcode));

		/* Failed, thus clean up */
		aprintf_free(l_captcha_image_path);
		l_captcha_image_path = NULL;

		return (NULL);
	}

	path = aprintf(
		"/rendezvous/file%s",
		l_captcha_image_path);
	if (path == NULL) {
		ret = NULL;
	} else {
		/* Our captcha image */
		ret = rdv_make_peel_response(path, "Here is your captcha image!");

		/* Free temporary path */
		aprintf_free(path);
	}

	if (ret == NULL) {
		/* Failed, thus clean up */
		aprintf_free(l_captcha_image_path);
		l_captcha_image_path = NULL;
	}

	return (ret);
}

static const char *
rdv_peel_captcha_with_image_path(json_t *root);
static const char *
rdv_peel_captcha_with_image_path(json_t *root) {
	json_t		*answer_val;
	const char	*r;

	/* Do they have an answer? */
	answer_val = json_object_get(root, "action");

	if (answer_val == NULL) {
		r = "Answer not found in JSON";

	} else if (json_is_string(answer_val)) {
		char	*answer;
		onion_t	inner_onion = NULL;
		int	defcode;

		answer = (char *)json_string_value(answer_val);

		defcode = peel_captcha_onion(answer, l_current_onion,
					     &inner_onion);

		if (defcode == DEFIANT_OK) {
			/* Free current onion */
			free_onion(l_current_onion);

			/* The new onion */
			l_current_onion = inner_onion;

			r = "Excellent, you solved the captcha";
		} else {
			r = "Nope, try again?";
		}
	} else {
		r = "JSON Answer field wasn't of the right type";
	}

	return (rdv_make_peel_response("", r));
}

static const char *
rdv_peel_captcha(json_t *root);
static const char *
rdv_peel_captcha(json_t *root) {
	log_dbg("...");

	return (l_captcha_image_path == NULL ?
		rdv_peel_captcha_no_image_path() :
		rdv_peel_captcha_with_image_path(root));
}

static const char *
rdv_peel_signed(void);
static const char *
rdv_peel_signed(void) {
  int 		errcode;
  const char	*r;
  const char	*dpkp;
  FILE            *public_key_fp;
  log_dbg("...");
  dpkp = getenv("DEFIANCE_PUBLIC_KEY_PATH");
  if (dpkp  == NULL) {
    r = "The server needs to know the DEFIANCE_PUBLIC_KEY_PATH, so  "
      "that the signature can be VERIFIED!";
  } else {
          
    public_key_fp = fopen(dpkp, "r");
    
    if(public_key_fp == NULL){
      log_wrn("Could not open public key file %s: %s\n",  dpkp, strerror(errno));
      r = "The server can't open the DEFIANCE_PUBLIC_KEY_PATH, so  "
        "that the signature can be VERIFIED!";
    } else {
      errcode = verify_onion(public_key_fp, l_current_onion);
      fclose(public_key_fp);
      if (errcode == DEFIANT_OK) {
        onion_t inner_onion = NULL;
        errcode = peel_signed_onion(l_current_onion, &inner_onion);

        if (errcode == DEFIANT_OK) {
          free_onion(l_current_onion);

          l_current_onion = inner_onion;

          r = "The server returned an onion whose "
            "signature we VERIFIED!";
        } else {
          r = "Peeling it went wrong, very odd.";
        }
      } else {
        r = "The server returned an onion whose signature "
          "we COULD NOT verify -- try again?";
      }
    }
  }

  return (rdv_make_peel_response("", r));
}

static void
rdv_peel(httpsrv_client_t *hcl);
static void
rdv_peel(httpsrv_client_t *hcl) {
	const char	*response = NULL;
	json_error_t	error;
	json_t		*root;
	int		otype;

	if (hcl->method != HTTP_M_POST) {
		djb_error(hcl, 400, "peel requires a POST");
		return;
	}

	/* No body yet? Then allocate some memory to get it */
	if (hcl->readbody == NULL) {
		if (hcl->headers.content_length == 0) {
			djb_error(hcl, 400, "peel requires length");
			return;
		}

		if (httpsrv_readbody_alloc(hcl, 0, 0) < 0) {
			log_dbg("httpsrv_readbody_alloc() failed");
		}
		return;
	}

	log_dbg("readbody: %s", hcl->readbody);
	root = json_loads(hcl->readbody, 0, &error);
	httpsrv_readbody_free(hcl);

	if (root == NULL) {
		log_dbg("libjansson error: line: %u msg: %s",
			error.line, error.text);
		djb_error(hcl, 400, "Invalid JSON");

	} else if (!json_is_object(root)) {
		log_dbg("JSON passed is not an object");
		djb_error(hcl, 400, "No Object in JSON");

	} else if (l_current_onion == NULL) {
		djb_error(hcl, 400, "Currently no onion");

	} else if (!ONION_IS_ONION(l_current_onion)) {
		djb_error(hcl, 400, "Onion is not an onion");

	} else {
		otype = ONION_TYPE(l_current_onion);
		log_dbg("onion_type: %s\n", rdv_onion_name(otype));

		switch (otype) {
		case BASE:
			response = rdv_peel_base();
			break;

		case POW:
			response = rdv_peel_pow();
			break;

		case CAPTCHA:
			response = rdv_peel_captcha(root);
			break;

		case SIGNED:
			response = rdv_peel_signed();
			break;

		case COLLECTION:
		default:
			djb_error(hcl, 400,
				 "Onion method not implemented yet");
			break;
		}

		if (response != NULL) {
			djb_presult(hcl, response);

			aprintf_free(response);
			response = NULL;
		} else {
			djb_error(hcl, 400, "Peeling failed");
		}
	}

	json_decref(root);
}

static void
rdv_file(httpsrv_client_t *hcl, const char *file);
static void
rdv_file(httpsrv_client_t *hcl, const char *file) {
  const char prefix[]  = "/tmp/jpeg_steg_embed";
	/* Only serve random jpeg_steg_embed files */
	log_dbg("file = %s", file);
	if (strncmp(file, prefix, sizeof(prefix) - 1) == 0) {
		httpsrv_sendfile(hcl, file);
		httpsrv_expire(hcl, HTTPSRV_EXPIRE_SHORT);

		/* XXX: As it has been fetched, we could delete it */
	} else {
		djb_error(hcl, HTTPSRV_HTTP_FORBIDDEN);
	}

	httpsrv_done(hcl);
}

/* Called from djb */
void
rdv_handle(httpsrv_client_t *hcl) {
	const char	*query;

	/* Skip '/rendezvous/' (12) */
	query = &(hcl->headers.uri[12]);

	log_dbg("query = %s", query);

	if (strcasecmp(query, "reset") == 0) {
		rdv_reset(hcl);

	} else if (strcasecmp(query, "gen_request") == 0) {
		rdv_gen_request(hcl);

	} else if (strcasecmp(query, "image") == 0) {
		rdv_image(hcl);

	} else if (strcasecmp(query, "peel") == 0) {
		rdv_peel(hcl);

	} else if (strncasecmp(query, "file/", 5) == 0) {
		rdv_file(hcl, &query[4]);

	} else {
		djb_error(hcl, 400, "No such DJB API request (Rendezvous)");
	}
}

