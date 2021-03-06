#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dnscache.h"
#include "dnsparser.h"
#include "dnsgenerator.h"
#include "utils.h"
#include "querydnsbase.h"
#include "rwlock.h"
#include "cacheht.h"
#include "cachettlcrtl.h"

#define	CACHE_VERSION		22

#define	CACHE_END	'\x0A'
#define	CACHE_START	'\xFF'

static BOOL				Inited = FALSE;

static RWLock			CacheLock;

static FileHandle		CacheFileHandle;
static MappingHandle	CacheMappingHandle;
static char				*MapStart;

static ThreadHandle		TTLCountdown_Thread;

static int32_t			CacheSize;
static BOOL				IgnoreTTL;

static int32_t			*CacheCount;

static volatile int32_t	*CacheEnd; /* Offset */

static CacheHT			*CacheInfo;

static CacheTtlCtrl		*TtlCtrl = NULL;

struct _Header{
	uint32_t	Ver;
	int32_t		CacheSize;
	int32_t		End;
	int32_t		CacheCount;
	CacheHT		ht;
	char		Comment[128 - sizeof(uint32_t) - sizeof(int32_t) - sizeof(int32_t) - sizeof(int32_t) - sizeof(CacheHT)];
};

static void DNSCacheTTLCountdown_Thread(void)
{
	register	int			loop;
	register	BOOL		GotMutex	=	FALSE;
	register	Cht_Node	*Node	=	NULL;

	register	Array		*ChunkList	=	&(CacheInfo -> NodeChunk);

	register	time_t		CurrentTime;

	while( Inited )
	{
		CurrentTime = time(NULL);
		loop = ChunkList -> Used - 1;

		Node = (Cht_Node *)Array_GetBySubscript(ChunkList, loop);

		while( Node != NULL )
		{
			if( Node -> TTL > 0 )
			{
				if( CurrentTime - Node -> TimeAdded >= Node -> TTL )
				{
					if(GotMutex == FALSE)
					{
						RWLock_WrLock(CacheLock);
						GotMutex = TRUE;
					}

					Node -> TTL = 0;

					*(char *)(MapStart + Node -> Offset) = 0xFD;

					CacheHT_RemoveFromSlot(CacheInfo, loop, Node);

					--(*CacheCount);

				}
			}

			Node = (Cht_Node *)Array_GetBySubscript(ChunkList, --loop);
		}

		if(GotMutex == TRUE)
		{
			if( ChunkList -> Used == 0 )
			{
				(*CacheEnd) = sizeof(struct _Header);
			} else {
				Node = (Cht_Node *)(Cht_Node *)Array_GetBySubscript(ChunkList, ChunkList -> Used - 1);
				(*CacheEnd) = Node -> Offset + Node -> Length;
			}

			RWLock_UnWLock(CacheLock);
			GotMutex = FALSE;
		}

		SLEEP(59000);
	}

	TTLCountdown_Thread = INVALID_THREAD;
}

static BOOL IsReloadable(void)
{
	struct _Header	*Header = (struct _Header *)MapStart;

	if( Header -> Ver != CACHE_VERSION )
	{
		ERRORMSG("The existing cache is not compatible with this version of program.\n");
		return FALSE;
	}

	if( Header -> CacheSize != CacheSize )
	{
		ERRORMSG("The size of the existing cache and the value of `CacheSize' should be equal.\n");
		return FALSE;
	}

	return TRUE;
}

static void ReloadCache(void)
{
	struct _Header	*Header = (struct _Header *)MapStart;

	INFO("Reloading the cache ...\n");

	CacheInfo = &(Header -> ht);

	CacheHT_ReInit(CacheInfo, MapStart, CacheSize);

	CacheEnd = &(Header -> End);
	CacheCount = &(Header -> CacheCount);

	INFO("Cache reloaded, containing %d entries for %d items.\n", CacheInfo -> NodeChunk.Used, (*CacheCount));
}

static void CreateNewCache(void)
{
	struct _Header	*Header = (struct _Header *)MapStart;

	memset(MapStart, 0, CacheSize);

	Header -> Ver = CACHE_VERSION;
	Header -> CacheSize = CacheSize;
	Header -> CacheCount = 0;
	CacheEnd = &(Header -> End);
	*CacheEnd = sizeof(struct _Header);
	memset(Header -> Comment, 0, sizeof(Header -> Comment));
	strcpy(Header -> Comment, "\nDo not edit this file.\n");

	CacheInfo = &(Header -> ht);
	CacheCount = &(Header -> CacheCount);

	CacheHT_Init(CacheInfo, MapStart, CacheSize);

}

static int InitCacheInfo(ConfigFileInfo *ConfigInfo, BOOL Reload)
{
	if( Reload == TRUE )
	{
		if( IsReloadable() )
		{
			ReloadCache();
		} else {
			if( ConfigGetBoolean(ConfigInfo, "OverwriteCache") == FALSE )
			{
				return -1;
			} else {
				CreateNewCache();
				INFO("The existing cache has been overwritten.\n");
			}
		}
	} else {
		CreateNewCache();
	}
	return 0;
}

int DNSCache_Init(ConfigFileInfo *ConfigInfo)
{
	int			_CacheSize = ConfigGetInt32(ConfigInfo, "CacheSize");
	const char	*CacheFile = ConfigGetRawString(ConfigInfo, "CacheFile");
	int			InitCacheInfoState;

	int			OverrideTTL;
	int			TTLMultiple;

	StringList	*ctc = ConfigGetStringList(ConfigInfo, "CacheControl");

	if( ConfigGetBoolean(ConfigInfo, "UseCache") == FALSE )
	{
		return 0;
	}

	IgnoreTTL = ConfigGetBoolean(ConfigInfo, "IgnoreTTL");

	OverrideTTL = ConfigGetInt32(ConfigInfo, "OverrideTTL");
	TTLMultiple = ConfigGetInt32(ConfigInfo, "MultipleTTL");

	if( ctc != NULL || OverrideTTL > -1 || TTLMultiple > 1 )
	{
		TtlCtrl = malloc(sizeof(CacheTtlCtrl));
		if( TtlCtrl == NULL || CacheTtlCrtl_Init(TtlCtrl) != 0 )
		{
			return -1;
		}
	}

	if( ctc != NULL )
	{
		CacheTtlCrtl_Add_From_StringList(TtlCtrl, ctc);
	}

	if( OverrideTTL > -1 )
	{
		CacheTtlCrtl_Add(TtlCtrl, "*", TTL_STATE_FIXED, 1, OverrideTTL, TRUE);
	} else {
		if( TTLMultiple < 1 )
		{
			ERRORMSG("Invalid `MultipleTTL'.\n");
		} else if( TTLMultiple > 1 ){
			CacheTtlCrtl_Add(TtlCtrl, "*", TTL_STATE_VARIABLE, TTLMultiple, 0, TRUE);
		}
	}

	if( _CacheSize % 8 != 0 )
	{
		CacheSize = ROUND_UP(_CacheSize, 8);
	} else {
		CacheSize = _CacheSize;
	}

	if( CacheSize < 102400 )
	{
		ERRORMSG("Cache size must not less than 102400 bytes.\n");
		return 1;
	}

	if( ConfigGetBoolean(ConfigInfo, "MemoryCache") == TRUE )
	{
		MapStart = SafeMalloc(CacheSize);

		if( MapStart == NULL )
		{
			ERRORMSG("Cache initializing failed.\n");
			return 2;
		}

		InitCacheInfoState = InitCacheInfo(ConfigInfo, FALSE);
	} else {
		BOOL FileExists;

		INFO("Cache File : %s\n", CacheFile);

		FileExists = FileIsReadable(CacheFile);

		CacheFileHandle = OPEN_FILE(CacheFile);
		if(CacheFileHandle == INVALID_FILE)
		{
			int ErrorNum = GET_LAST_ERROR();
			char ErrorMessage[320];

			GetErrorMsg(ErrorNum, ErrorMessage, sizeof(ErrorMessage));

			ERRORMSG("Cache initializing failed : %d : %s.\n", ErrorNum, ErrorMessage);

			return 3;
		}

		CacheMappingHandle = CREATE_FILE_MAPPING(CacheFileHandle, CacheSize);
		if(CacheMappingHandle == INVALID_MAP)
		{
			int ErrorNum = GET_LAST_ERROR();
			char ErrorMessage[320];

			GetErrorMsg(ErrorNum, ErrorMessage, sizeof(ErrorMessage));

			ERRORMSG("Cache initializing failed : %d : %s.\n", ErrorNum, ErrorMessage);
			return 4;
		}

		MapStart = (char *)MPA_FILE(CacheMappingHandle, CacheSize);
		if(MapStart == INVALID_MAPPING_FILE)
		{
			int ErrorNum = GET_LAST_ERROR();
			char ErrorMessage[320];

			GetErrorMsg(ErrorNum, ErrorMessage, sizeof(ErrorMessage));

			ERRORMSG("Cache initializing failed : %d : %s.\n", ErrorNum, ErrorMessage);
			return 5;
		}

		if( FileExists == FALSE )
		{
			InitCacheInfoState = InitCacheInfo(ConfigInfo, FALSE);
		} else {
			InitCacheInfoState = InitCacheInfo(ConfigInfo, ConfigGetBoolean(ConfigInfo, "ReloadCache"));
		}
	}

	if( InitCacheInfoState != 0 )
	{
		return 6;
	}

	RWLock_Init(CacheLock);

	Inited = TRUE;

	if(IgnoreTTL == FALSE)
		CREATE_THREAD(DNSCacheTTLCountdown_Thread, NULL, TTLCountdown_Thread);

	return 0;
}

BOOL Cache_IsInited(void)
{
	return Inited;
}

static int32_t DNSCache_GetAviliableChunk(uint32_t Length, Cht_Node **Out)
{
	int32_t	NodeNumber;
	Cht_Node	*Node;
	uint32_t	RoundedLength = ROUND_UP(Length, 8);

	BOOL	NewCreated;

	NodeNumber = CacheHT_FindUnusedNode(CacheInfo, RoundedLength, &Node, MapStart + (*CacheEnd) + RoundedLength, &NewCreated);
	if( NodeNumber >= 0 )
	{
		if( NewCreated == TRUE )
		{
			Node -> Offset = (*CacheEnd);
			(*CacheEnd) += RoundedLength;
		}

		memset(MapStart + Node -> Offset + Length, 0xFE, RoundedLength - Length);

		*Out = Node;
		return NodeNumber;
	} else {
		*Out = NULL;
		return -1;
	}

}

static Cht_Node *DNSCache_FindFromCache(char *Content, size_t Length, Cht_Node *Start, time_t CurrentTime)
{
	Cht_Node *Node = Start;

	do{
		Node = CacheHT_Get(CacheInfo, Content, Node, NULL);
		if( Node == NULL )
		{
			return NULL;
		}

		if( IgnoreTTL == TRUE || (CurrentTime - Node -> TimeAdded < Node -> TTL) )
		{
			if( memcmp(Content, MapStart + Node -> Offset + 1, Length) == 0 )
			{
				return Node;
			}
		}

	} while( TRUE );

}

static int DNSCache_AddAItemToCache(DnsSimpleParserIterator *i, time_t CurrentTime, const CtrlContent *InfectedTtlContent)
{
	/* used to store cache data temporarily, TODO: no bounds checking here */
	char			Buffer[512];
	char			*HostName = Buffer + 1;

	/* Iterator of `Buffer' */
	char			*BufferItr = Buffer;

	const CtrlContent	*TtlContent;

	/* Assign start byte of the cache */
	Buffer[0] = CACHE_START;

	/* Assign the name of the cache */
	if( i->GetName(i, HostName, sizeof(Buffer) -1) < 0 )
	{
		return -1;
	}

	/* Detemine which TTL scheme will be used */
	if( InfectedTtlContent != NULL )
	{
		switch( InfectedTtlContent -> Infection )
		{
			default:
			case TTL_CTRL_INFECTION_AGGRESSIVLY:
				TtlContent = InfectedTtlContent;
				break;

			case TTL_CTRL_INFECTION_PASSIVLY:
				TtlContent = CacheTtlCrtl_Get(TtlCtrl, HostName);
				if( TtlContent == NULL )
				{
					TtlContent = InfectedTtlContent;
				}
				break;

			case TTL_CTRL_INFECTION_NONE:
				TtlContent = CacheTtlCrtl_Get(TtlCtrl, HostName);
				break;
		}
	} else {
		TtlContent = CacheTtlCrtl_Get(TtlCtrl, HostName);
	}

	/* Jump just over the name, right at '\0' */
	BufferItr = HostName + strlen(HostName);
    if( BufferItr >= Buffer + sizeof(Buffer) )
    {
        return -2;
    }

	/* Set record type and class */
	BufferItr += snprintf(BufferItr,
                          sizeof(Buffer) - (BufferItr - Buffer),
                          "\1%d\1%d",
                          i->Type,
                          i->Klass
                          );
    if( BufferItr >= Buffer + sizeof(Buffer) )
    {
        return -3;
    }

	/* End of name \1 type \1 class boundle */
	*BufferItr++ = '\0';
    if( BufferItr >= Buffer + sizeof(Buffer) )
    {
        return -4;
    }

	/* Generate data and store them */
    if( i->TextifyData(i,
                       "%v",
                       BufferItr,
                       sizeof(Buffer) - (BufferItr - Buffer)
                       ) <= 0 )
    {
        return -5;
    }
	BufferItr += strlen(BufferItr) + 1;
    if( BufferItr >= Buffer + sizeof(Buffer) )
    {
        return -6;
    }

	/* Mark the end */
	*BufferItr = CACHE_END;

	/* The whole cache data generating completed */

	/* Add the cache item to the main cache zone below */

	/* Determine whether the cache item has existed in the main cache zone */
	if(DNSCache_FindFromCache(Buffer + 1, BufferItr - Buffer, NULL, CurrentTime) == NULL)
	{
		/* If not, add it */

		/* Subscript of a chunk in the main cache zone */
		int32_t	Subscript;

		uint32_t RecordTTL;

		/* Node with subscript `Subscript' */
		Cht_Node	*Node;

		if( TtlContent != NULL )
		{
			switch( TtlContent -> State )
			{
				case TTL_STATE_NO_CACHE:
					RecordTTL = 0;
					break;

				case TTL_STATE_ORIGINAL:
					RecordTTL = i->GetTTL(i);
					break;

				default:
					RecordTTL = (TtlContent -> Coefficient) * i->GetTTL(i) + (TtlContent -> Increment);
					break;
			}
		} else {
			RecordTTL = i->GetTTL(i);
		}

		if( RecordTTL == 0 )
		{
			return 0;
		}

		/* Get a usable chunk and its subscript */
		Subscript = DNSCache_GetAviliableChunk(BufferItr - Buffer + 1, &Node);

		/* If there is a usable chunk */
		if(Subscript >= 0)
		{
			/* Copy the cache to this entry */
			memcpy(MapStart + Node -> Offset, Buffer, BufferItr - Buffer + 1);

			/* Assign TTL */
			Node -> TTL = RecordTTL;

			Node -> TimeAdded = CurrentTime;

			/* Index this entry on the hash table */
			CacheHT_InsertToSlot(CacheInfo, Buffer + 1, Subscript, Node, NULL);

			++(*CacheCount);
		} else {
			return -1;
		}
	}

	return 0;
}

int DNSCache_AddItemsToCache(char *DNSBody, int DNSBodyLength, time_t CurrentTime, const char *Domain)
{
	const CtrlContent *TtlContent = NULL;

	DnsSimpleParser p;
	DnsSimpleParserIterator i;

	if(Inited == FALSE) return 0;

	if( DnsSimpleParser_Init(&p, DNSBody, DNSBodyLength, FALSE) != 0 )
    {
        return -1;
    }

	if( DnsSimpleParserIterator_Init(&i, &p) != 0 )
    {
        return -2;
    }

	TtlContent =  CacheTtlCrtl_Get(TtlCtrl, Domain);
	RWLock_WrLock(CacheLock);

    while( i.Next(&i) != NULL )
    {
        BOOL RightPurpose = i.Purpose != DNS_RECORD_PURPOSE_UNKNOWN &&
                            i.Purpose != DNS_RECORD_PURPOSE_QUESTION;

        BOOL CachedType = i.Type == DNS_TYPE_A ||
                          i.Type == DNS_TYPE_AAAA ||
                          i.Type == DNS_TYPE_CNAME;

        BOOL CachedClass = i.Klass == DNS_CLASS_IN;

        if( RightPurpose && CachedType && CachedClass )
        {
            DNSCache_AddAItemToCache(&i, CurrentTime, TtlContent);
        }
    }

	RWLock_UnWLock(CacheLock);

	return 0;
}

static int DNSCache_GenerateDatasFromCache(	__in			char				*Datas,
											__inout			char				*Buffer,
											__in			int					BufferLength,
											__in	const	ElementDescriptor	*Descriptor,
											__in			int					CountOfDescriptor
											)
{
	int		TotleLength = 0;
	int		SingleLength;
	int		loop;

	for(loop = 0; loop != CountOfDescriptor && *Datas != CACHE_END; ++loop)
	{
		SingleLength = DNSGenerateData(Datas, NULL, 0, Descriptor + loop);

		if( BufferLength < SingleLength )
		{
			break;
		}

		DNSGenerateData(Datas, Buffer, SingleLength, Descriptor + loop);

		TotleLength += SingleLength;
		BufferLength -= SingleLength;
		Buffer += SingleLength;

		/* move to next record */
		for(;*Datas != '\0'; ++Datas);
		++Datas;
	}

	return TotleLength;
}

static int DNSCache_GetRawRecordsFromCache(	__in	char				*Name,
											__in	DNSRecordType		Type,
											__in	DNSRecordClass		Class,
											__inout char				*Buffer,
											__in	int					BufferLength,
											__out	int					*RecordsLength,
											__in	time_t				CurrentTime
											)
{
	int			SingleLength;
	char		*CacheItr;
	Cht_Node	*Node = NULL;

	int			DatasLen;

	uint32_t	NewTTL;

	int 		RecordCount = 0;

	const ElementDescriptor *Descriptor;
	int CountOfDescriptor;

	char Name_Type_Class[256];

	*RecordsLength = 0;

	sprintf(Name_Type_Class, "%s\1%d\1%d", Name, Type, Class);

	do
	{
		Node = DNSCache_FindFromCache(Name_Type_Class, strlen(Name_Type_Class) + 1, Node, CurrentTime);
		if( Node == NULL )
		{
			break;
		}

		if( Node -> TTL != 0 )
		{
			CountOfDescriptor = DNSGetDescriptor((DNSRecordType)Type, TRUE, &Descriptor);
			if(CountOfDescriptor != 0)
			{
				++RecordCount;

				CacheItr = MapStart + Node -> Offset + 1;

				SingleLength = DNSGenResourceRecord(NULL, 0, "a", Type, Class, 0, NULL, 0, FALSE);

				if( BufferLength < SingleLength )
				{
					break;
				}

				if( IgnoreTTL == TRUE )
				{
					NewTTL = Node -> TTL;
				} else {
					NewTTL = Node -> TTL - (CurrentTime - Node -> TimeAdded);
				}

				DNSGenResourceRecord(Buffer, SingleLength, "a", Type, Class, NewTTL, NULL, 0, FALSE);

				for(; *CacheItr != '\0'; ++CacheItr);
				/* Now *CacheItr == '\0' */
				++CacheItr;
				/* Now the data position */

				Buffer += SingleLength;
				BufferLength -= SingleLength;

				DatasLen = DNSCache_GenerateDatasFromCache(CacheItr, Buffer, BufferLength, Descriptor, CountOfDescriptor);

				SET_16_BIT_U_INT(Buffer - 2, DatasLen);
				Buffer += DatasLen;

				(*RecordsLength) += (SingleLength + DatasLen);

			}
		}
	} while ( TRUE );

	return RecordCount;
}

static Cht_Node *DNSCache_GetCNameFromCache(__in char *Name, __out char *Buffer, __in time_t CurrentTime)
{
	char Name_Type_Class[256];
	Cht_Node *Node = NULL;

	sprintf(Name_Type_Class, "%s\1%d\1%d", Name, DNS_TYPE_CNAME, 1);

	do
	{
		Node = DNSCache_FindFromCache(Name_Type_Class, strlen(Name_Type_Class) + 1, Node, CurrentTime);
		if( Node == NULL )
		{
			return NULL;
		}

		strcpy(Buffer, MapStart + Node -> Offset + 1 + strlen(Name_Type_Class) + 1);
		return Node;

	} while( TRUE );

}

static int DNSCache_GetByQuestion(__in const char *Question, __in int QuestionLength, __inout char *Buffer, __in int BufferLength, __out int *RecordsLength, __in time_t CurrentTime)
{
	int		SingleLength	=	0;
	char	Name[260];
	char	CName[260];

	Cht_Node *Node;

	uint32_t	NewTTL;

	int		RecordsCount	=	0;

	DNSRecordType	Type;
	DNSRecordClass	Class;

	if(Inited == FALSE) return -1;

	*RecordsLength = 0;

	if( DNSGetHostName(Question, QuestionLength, DNSJumpHeader(Question), Name, sizeof(Name)) < 0 )
	{
		return -1;
	}

	Type = (DNSRecordType)DNSGetRecordType(DNSJumpHeader(Question));
	Class = (DNSRecordClass)DNSGetRecordClass(DNSJumpHeader(Question));


	RWLock_RdLock(CacheLock);

	/* If the intended type is not DNS_TYPE_CNAME, then first find its cname */
	if(Type != DNS_TYPE_CNAME)
	{
		while( (Node = DNSCache_GetCNameFromCache(Name, CName, CurrentTime)) != NULL )
		{

			SingleLength = DNSGenResourceRecord(NULL, 0, "a", DNS_TYPE_CNAME, 1, 0, CName, strlen(CName) + 1, TRUE);

			if( BufferLength < SingleLength )
			{
			    RWLock_UnRLock(CacheLock);
				return RecordsCount;
			}

			++RecordsCount;

			if( IgnoreTTL == TRUE )
			{
				NewTTL = Node -> TTL;
			} else {
				NewTTL = Node -> TTL - (CurrentTime - Node -> TimeAdded);
			}

			DNSGenResourceRecord(Buffer, SingleLength, "a", DNS_TYPE_CNAME, 1, NewTTL, CName, strlen(CName) + 1, TRUE);

			BufferLength -= SingleLength;
			Buffer += SingleLength;

			(*RecordsLength) += SingleLength;

			strcpy(Name, CName);
		}
	}

	RecordsCount += DNSCache_GetRawRecordsFromCache(Name, Type, Class, Buffer, BufferLength, &SingleLength, CurrentTime);

	RWLock_UnRLock(CacheLock);

	if( RecordsCount == 0 || SingleLength == 0 )
	{
		return 0;
	}

	(*RecordsLength) += SingleLength;

	return RecordsCount;
}

int DNSCache_FetchFromCache(char *RequestContent, int RequestLength, int BufferLength)
{
	BOOL	EDNSEnabled;
	int		RecordsCount, RecordsLength;

	if( Inited == FALSE )
	{
		return -1;
	}

	switch( DNSRemoveEDNSPseudoRecord(RequestContent, &RequestLength) )
	{
		case EDNS_REMOVED:
			EDNSEnabled = TRUE;
			break;

		case EDNS_NO_AR:
			EDNSEnabled = FALSE;
			break;

		default:
			return -1;
	}

	RecordsCount = DNSCache_GetByQuestion(RequestContent, RequestLength, RequestContent + RequestLength, BufferLength - RequestLength, &RecordsLength, time(NULL));
	if( RecordsCount > 0 )
	{
		int UnCompressedLength = RequestLength + RecordsLength;
		int CompressedLength;

		((DNSHeader *)RequestContent) -> AnswerCount = htons(RecordsCount);

		CompressedLength = DNSCompress(RequestContent, UnCompressedLength);

		((DNSHeader *)RequestContent) -> Flags.Direction = 1;
		((DNSHeader *)RequestContent) -> Flags.AuthoritativeAnswer = 0;
		((DNSHeader *)RequestContent) -> Flags.RecursionAvailable = 1;
		((DNSHeader *)RequestContent) -> Flags.ResponseCode = 0;
		((DNSHeader *)RequestContent) -> Flags.Type = 0;

		if( EDNSEnabled == TRUE )
		{
			DNSAppendEDNSPseudoRecord(RequestContent, &CompressedLength);
		}

		return CompressedLength;
	} else {
		if( EDNSEnabled == TRUE )
		{
			DNSAppendEDNSPseudoRecord(RequestContent, &RequestLength);
		}
		return -1;
	}
}

void DNSCacheClose(ConfigFileInfo *ConfigInfo)
{
	if(Inited == TRUE)
	{
		Inited = FALSE;
		RWLock_WrLock(CacheLock);

		if( ConfigGetBoolean(ConfigInfo, "MemoryCache") == FALSE )
		{
			UNMAP_FILE(MapStart, CacheSize);
			DESTROY_MAPPING(CacheMappingHandle);
			CLOSE_FILE(CacheFileHandle);
		} else {
			SafeFree(MapStart);
		}

		RWLock_UnWLock(CacheLock);
		RWLock_Destroy(CacheLock);
	}
}
