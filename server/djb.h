#ifndef SHARED_H
#define SHARED_H 1

#if __llvm__
// Workaround DEPRECATED_IN_MAC_OS_X_VERSION_10_7_AND_LATER
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

/* LibFUtil - FUnctions and UTILities */
#include <libfutil/httpsrv.h>
#include <libfutil/list.h>

/* Jansson - the JSON parser */
#include <jansson.h>

typedef bool (*djb_push_f)(httpsrv_client_t *shcl, httpsrv_client_t *hcl);

typedef struct {
	/* Push Callback */
	djb_push_f	push;

	char		httpcode[64];
	char		httptext[(256-64-32)];
	char		seqno[32];
	char		setcookie[8192];
	char		cookie[8192];
} djb_headers_t;

typedef enum
{
	DJB_ERR = 0,
	DJB_OK,
	DJB_DONE,
	DJB_MAX
} djb_status_t;

#define DJB_MSGLEN 256

/* DJB provided functions */
void djb_error(httpsrv_client_t *hcl, unsigned int errcode, const char *msg);
void djb_presult(httpsrv_client_t *hcl, const char *msg);
void djb_result(httpsrv_client_t *hcl, djb_status_t status, const char *msg);

bool djb_proxy_add(httpsrv_client_t *hcl);
djb_headers_t *djb_create_userdata(httpsrv_client_t *hcl);

/* ACS API */
void acs_init(httpsrv_t *hs);
void acs_exit(void);
bool acs_handle(httpsrv_client_t *hcl);
bool acs_set_net(json_t *net_);

/* Rendezvous API */
void rdv_handle(httpsrv_client_t *hcl);

/* Preferences API */
enum prf_v {
	PRF_CC = 0,
	PRF_EXE,
	PRF_LL,
	PRF_SM,
	PRF_TP,
	PRF_SHS,
	PRF_PA,
	PRF_JA,
	PRF_SA,
	PRF_MAX         /* Maximum argument */
};

void prf_init(void);
void prf_exit(void);
void prf_handle(httpsrv_client_t *hcl);
bool prf_set_bridge_access_list(const char *br);
int prf_get_argv(char **argv[]);
void prf_free_argv(unsigned int argc, char *argv[]);
const char *prf_get_value(enum prf_v i);

#endif /* SHARED_H */
