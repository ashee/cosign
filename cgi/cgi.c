/*
 * Copyright (c) 2002 Regents of The University of Michigan.
 * All Rights Reserved.  See LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <krb5.h>

#include <openssl/ssl.h>
#include <snet.h>
#include "cgi.h"
#include "cosigncgi.h"
#include "network.h"

#define ERROR_HTML	"../templates/error.html"
#define LOGIN_HTML	"../templates/login.html"
#define SERVICE_MENU	"../templates/service-menu.html"
#define TKT_PREFIX	"/ticket/"
#define SIDEWAYS        1

extern char	*version;
char	*err = NULL, *ref = NULL, *service = NULL;
char	*title = "Authentication Required";
char	*host = _COSIGN_HOST;
char	*keytab_path = _KEYTAB_PATH;
int	port = 6663;

struct cgi_list cl[] = {
#define CL_UNIQNAME	0
        { "uniqname", NULL },
#define CL_PASSWORD	1
        { "password", NULL },
#define CL_REF		2
        { "ref", NULL },
#define CL_SERVICE	3
        { "service", NULL },
        { NULL, NULL },
};

void            (*logger)( char * ) = NULL;
void            subfile( char * );

    void
subfile( char *filename )
{
    FILE	*fs;
    int 	c, i;
    char	nasties[] = "<>(){}[];'`\" \\";

    fputs( "Cache-Control: no-cache, private\n"
	    "Pragma: no-cache\n"
	    "Expires: Mon, 16 Apr 1973 13:10:00 GMT\n"
	    "Content-type: text/html\n\n", stdout );

    if (( fs = fopen( filename, "r" )) == NULL ) {
	perror( filename );
	exit( SIDEWAYS );
    }

    while (( c = getc( fs )) != EOF ) {
	if ( c == '$' ) {

	    switch ( c = getc( fs )) {
            case 'c':
                if ( service != NULL ) {
                    for ( i = 0; i < strlen( service ); i++ ) {
                        /* block XSS attacks while printing */
                        if ( strchr( nasties, service[ i ] ) != NULL ||
                                service[ i ] <= 0x1F || service[ i ] >= 0x7F ) {

			    printf( "%%%x", service[ i ] );
                        } else {
                            putc( service[ i ], stdout );
                        }
                    }
                }
                break;

	    case 't':
		if ( title != NULL ) {
		    printf( "%s", title );
		}
		break;

	    case 'e':
		if ( err != NULL ) {
		    printf( "%s", err );
		}
		break;

	    case 'u':
                if (( cl[ CL_UNIQNAME ].cl_data != NULL ) &&
                        ( *cl[ CL_UNIQNAME ].cl_data != '\0' )) {
                    printf( "%s", cl[ CL_UNIQNAME ].cl_data );
                }
		break;

	    case 's':
		printf( "%s", getenv( "SCRIPT_NAME" ));
		break;

	    case 'h':
		printf( "%s", host );
		break;

            case 'k':
		break;

            case 'r':
                if ( ref != NULL ) {
                    for ( i = 0; i < strlen( ref ); i++ ) {
                        /* block XSS attacks while printing */
                        if ( strchr( nasties, ref[ i ] ) != NULL ||
                                ref[ i ] <= 0x1F || ref[ i ] >= 0x7F ) {

			    printf( "%%%x", ref[ i ] );
                        } else {
                            putc( ref[ i ], stdout );
                        }
                    }
                }
                break;

	    case EOF:
		putchar( '$' );
		break;

	    case '$':
		putchar( c );
		break;

	    default:
		putchar( '$' );
		putchar( c );
	    }
	} else {
	    putchar( c );
	}
    }

    if ( fclose( fs ) != 0 ) {
	perror( filename );
    }

    return;
}


    int
main( int argc, char *argv[] )
{
    krb5_error_code		kerror;
    krb5_context		kcontext;
    krb5_principal		kprinc;
    krb5_principal		sprinc;
    krb5_get_init_creds_opt	kopts;
    krb5_creds			kcreds;
    krb5_ccache			kccache;
    krb5_keytab			keytab = 0;
    char			ktbuf[ MAX_KEYTAB_NAME_LEN + 1 ];
    int				rc;
    char                	new_cookiebuf[ 128 ];
    char        		new_cookie[ 255 ];
    char               		tmpkrb[ 16 ], krbpath [ 24 ];
    char			*data, *ip_addr;
    char			*cookie = NULL, *method, *script, *qs;
    char			*tmpl = LOGIN_HTML;

    if ( argc == 2 && ( strncmp( argv[ 1 ], "-V", 2 ) == 0 )) {
	printf( "%s\n", version );
	exit( 0 );
    }

    if (( data = getenv( "HTTP_COOKIE" )) != NULL ) {
	cookie = strtok( data, ";" );

	/* nibble away the cookie string until we see the cosign= cookie */
	if ( strncmp( cookie, "cosign=", 7 ) != 0 ) {
	    while (( cookie = strtok( NULL, ";" )) != NULL ) {
		if ( *cookie == ' ' ) ++cookie;
		if ( strncmp( cookie, "cosign=", 7 ) == 0 ) {
		    break;
		}
	    }
	}
    }

    method = getenv( "REQUEST_METHOD" );
    script = getenv( "SCRIPT_NAME" );
    ip_addr = getenv( "REMOTE_ADDR" );

    if ((( qs = getenv( "QUERY_STRING" )) != NULL ) && ( *qs != '\0' )) {
	service = strtok( qs, ";" );
	ref = strtok( NULL, "&" );

	if ( cookie == NULL || strlen( cookie ) == 7 ) {
	    title = "Authentication Required";
	    err = "Something about coming in from the top.";
	    goto loginscreen;
	}

	if ( strncmp( service, "cosign-", 7 ) != 0 ) {
	    title = "Error: Unrecognized Service";
	    tmpl = ERROR_HTML;
	    err = "Unable to determine referring service from query string.";
	    subfile( tmpl );
	    exit( 0 );
	}

	if ( strlen( service ) > MAXPATHLEN ) {
	    tmpl = ERROR_HTML;
	    title = "Error: Max Length Exceeded";
	    err = "An error occurred while processing your request:  max length exceeded.";
	    subfile( tmpl );
	    exit( 0 );
	}

	if (( rc = cosign_register( cookie, ip_addr, service )) < 0 ) {
	    if ( cosign_check( cookie ) < 0 ) {
		err = "You are not logged in. Please log in now.";
		goto loginscreen;
	    }

	    fprintf( stderr, "%s: cosign_register failed\n", script );
	    title = "Error: Register Failed";
	    tmpl = ERROR_HTML;
	    err = "We were unable to contact the authentication server.  Please try again later.";
	    subfile( tmpl );
	    exit( 0 );
	}

	if ( rc > 0 ) {
	    err = "You are not logged in.  Please log in now.";
	    goto loginscreen;
	}

	/* if no referrer, redirect to top of site from conf file */
	printf( "Location: %s\n\n", ref );
	exit( 0 );
    }

    if ( cookie == NULL ) {
	if ( strcmp( method, "POST" ) == 0 ) {
	    title = "Error: Cookies Required";
	    err = "This service requires that cookies be enabled.";
	    tmpl = ERROR_HTML;
	    subfile( tmpl );
	    exit( 0 );
	}
	goto loginscreen;
    }

    if ( strcmp( method, "POST" ) != 0 ) {
	if ( cosign_check( cookie ) < 0 ) {
	    err = "You are not logged in. Please log in now.";
	    goto loginscreen;
	}

	title = "Authentication Successful";
	tmpl = SERVICE_MENU;

	subfile( tmpl );
	exit( 0 );
    }

    if ( cgi_info( CGI_STDIN, cl ) != 0 ) {
	exit( SIDEWAYS );
    }

    if (( cl[ CL_REF ].cl_data != NULL ) ||
	    ( *cl[ CL_REF ].cl_data != '\0' )) {
        ref = cl[ CL_REF ].cl_data;
    }

    if (( cl[ CL_UNIQNAME ].cl_data == NULL ) ||
	    ( *cl[ CL_UNIQNAME ].cl_data == '\0' )) {
	title = "Authentication Required";
	err = "Please enter your uniqname and password.";
        subfile ( tmpl );
	exit( 0 );
    }

    if (( cl[ CL_PASSWORD ].cl_data == NULL ) ||
	    ( *cl[ CL_PASSWORD ].cl_data == '\0' )) {
	err = "Unable to login because password is a required field.";
	title = "Authentication Required ( missing password )";

        subfile ( tmpl );
	exit( 0 );
    }

    if (( kerror = krb5_init_context( &kcontext ))) {
	err = (char *)error_message( kerror );
	title = "Authentication Required ( kerberos error )";

	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }


    if (( kerror = krb5_parse_name( kcontext, cl[ CL_UNIQNAME ].cl_data,
	    &kprinc ))) {
	err = (char *)error_message( kerror );
	title = "Authentication Required ( kerberos error )";

	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }

    if ( mkcookie( sizeof( tmpkrb ), tmpkrb ) != 0 ) {
	err = "An unknown error occurred.";
	title = "Authentication Required ( kerberos error )";

	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }
    snprintf( krbpath, sizeof( krbpath ), "%s/%s", TKT_PREFIX, tmpkrb );

    if (( kerror = krb5_cc_resolve( kcontext, krbpath, &kccache )) != 0 ) {
	err = (char *)error_message( kerror );
	title = "Authentication Required ( kerberos error )";

	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }

    krb5_get_init_creds_opt_init( &kopts );
    krb5_get_init_creds_opt_set_tkt_life( &kopts, 10*60*60 );
    krb5_get_init_creds_opt_set_renew_life( &kopts, 0 );
    krb5_get_init_creds_opt_set_forwardable( &kopts, 1 );
    krb5_get_init_creds_opt_set_proxiable( &kopts, 0 );
    krb5_get_init_creds_opt_set_address_list( &kopts, NULL );

    if (( kerror = krb5_get_init_creds_password( kcontext, &kcreds, 
	    kprinc, cl[ CL_PASSWORD ].cl_data, krb5_prompter_posix, NULL, 0, 
	    NULL /*keytab */, &kopts ))) {

	if ( kerror == KRB5KRB_AP_ERR_BAD_INTEGRITY ) {

	    err = "Password incorrect.  Is [caps lock] on?";
	    title = "Authentication Required ( Password Incorrect )";

	    tmpl = LOGIN_HTML;
	    subfile ( tmpl );
	    exit( 0 );
	} else {
	    err = (char *)error_message( kerror );
	    title = "( Password Error )";
	    
	    tmpl = ERROR_HTML;
	    subfile ( tmpl );
	    exit( 0 );
	}
    }

    /* verify no KDC spoofing */
    if ( *keytab_path == '\0' ) {
	if (( kerror = krb5_kt_default_name(
		kcontext, ktbuf, MAX_KEYTAB_NAME_LEN )) != 0 ) {
	    err = (char *)error_message( kerror );
	    title = "( Ticket Verify Error )";
	
	    tmpl = ERROR_HTML;
	    subfile ( tmpl );
	    exit( 0 );

	}
    } else {
	if ( strlen( keytab_path ) > MAX_KEYTAB_NAME_LEN ) {
	    err = "server configuration error";
	    title = "( Ticket Verify Error )";
    
	    tmpl = ERROR_HTML;
	    subfile ( tmpl );
	    exit( 0 );
	}
	strcpy( ktbuf, keytab_path );
    }

    if (( kerror = krb5_kt_resolve( kcontext, ktbuf, &keytab )) != 0 ) {
	err = (char *)error_message( kerror );
	title = "( KT Resolve Error )";
	
	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }

    if (( kerror = krb5_sname_to_principal( kcontext, NULL, "cosign",
	    KRB5_NT_SRV_HST, &sprinc )) != 0 ) {
	err = (char *)error_message( kerror );
	title = "( Server Principal Error )";
	
	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }

    if (( kerror = krb5_verify_init_creds(
	    kcontext, &kcreds, sprinc, keytab, NULL, NULL )) != 0 ) {
	err = (char *)error_message( kerror );
	title = "( Ticket Verify Error )";
	
	tmpl = ERROR_HTML;
	subfile ( tmpl );
	krb5_free_principal( kcontext, sprinc );
	exit( 0 );
    }
    (void)krb5_kt_close( kcontext, keytab );
    krb5_free_principal( kcontext, sprinc );

    if (( kerror = krb5_cc_initialize( kcontext, kccache, kprinc )) != 0 ) {
	err = (char *)error_message( kerror );
	title = "( Ticket Sticking Error )";
	
	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }

    if (( kerror = krb5_cc_store_cred( kcontext, kccache, &kcreds )) != 0 ) {
	err = (char *)error_message( kerror );
	title = "( Ticket Storing Error )";
	
	tmpl = ERROR_HTML;
	subfile ( tmpl );
	exit( 0 );
    }

    krb5_free_cred_contents( kcontext, &kcreds );
    krb5_free_principal( kcontext, kprinc );
    krb5_cc_close( kcontext, kccache );
    krb5_free_context( kcontext );
    /* have name and give that to login */

    /* password has been accepted, tell cosignd */
    err = "Your password has been accepted.";
    title = "Choose a Service";
    tmpl = SERVICE_MENU;

    if ( cosign_login( cookie, ip_addr, 
	    cl[ CL_UNIQNAME ].cl_data, "UMICH.EDU", krbpath ) < 0 ) {
	fprintf( stderr, "%s: login failed\n", script ) ;

	/* redirecting to service menu because user is logged in */
	if (( ref == NULL ) || ( ref = strstr( ref, "http" )) == NULL ) {
	    printf( "Location: %s\n\n", host );
	    exit( 0 );
	}
    }

    if (( cl[ CL_SERVICE ].cl_data != NULL ) &&
	    ( *cl[ CL_SERVICE ].cl_data != '\0' )) {

	/* url decode here the service cookie? */

        if (( rc = cosign_register( cookie, ip_addr,
		cl[ CL_SERVICE ].cl_data )) < 0 ) {

	    /* this should not be possible... do it anyway? */
            if ( cosign_check( cookie ) < 0 ) {
                title = "Authentication Required";
                goto loginscreen;
            }

            fprintf( stderr, "%s: implicit cosign_register failed\n", script );
            title = "Error: Implicit Register Failed";
            tmpl = ERROR_HTML;
            err = "We were unable to contact the authentication server.  Please try again later.";
            subfile( tmpl );
            exit( 0 );
        }
    }

    if (( ref != NULL ) && ( ref = strstr( ref, "http" )) != NULL ) {
	printf( "Location: %s\n\n", ref );
	exit( 0 );
    }

    subfile( tmpl );
    exit( 0 );

loginscreen:
    if ( mkcookie( sizeof( new_cookiebuf ), new_cookiebuf ) != 0 ) {
	fprintf( stderr, "%s: mkcookie: failed\n", script );
	exit( SIDEWAYS );
    }

    snprintf( new_cookie, sizeof( new_cookie ), "cosign=%s", new_cookiebuf );
    printf( "Set-Cookie: %s; path=/; secure\n", new_cookie );
    subfile( tmpl );
    exit( 0 );
}
