/* cdwWebBrowse - Browse CIRM data warehouse.. */

/* Copyright (C) 2014 The Regents of the University of California 
 * See README in this or parent directory for licensing information. */
#include "common.h"
#include "linefile.h"
#include "hash.h"
#include "options.h"
#include "obscure.h"
#include "cheapcgi.h"
#include "sqlSanity.h"
#include "htmshell.h"
#include "fieldedTable.h"
#include "portable.h"
#include "paraFetch.h"
#include "tagStorm.h"
#include "rql.h"
#include "cart.h"
#include "cdw.h"
#include "cdwLib.h"
#include "cdwValid.h"
#include "hui.h"
#include "hgColors.h"
#include "web.h"
#include "tablesTables.h"
#include "jsHelper.h"

/* Global vars */
struct cart *cart;	// User variables saved from click to click
struct hash *oldVars;	// Previous cart, before current round of CGI vars folded in

void usage()
/* Explain usage and exit. */
{
errAbort(
  "cdwWebBrowse is a cgi script not meant to be run from command line.\n"
  );
}

struct dyString *printPopularTags(struct hash *hash, int maxSize)
/* Get all hash elements, sorted by count, and print all the ones that fit */
{
maxSize -= 3;  // Leave room for ...
struct dyString *dy = dyStringNew(0);

struct hashEl *hel, *helList = hashElListHash(hash);
slSort(&helList, hashElCmpIntValDesc);
for (hel = helList; hel != NULL; hel = hel->next)
    {
    int oldSize = dy->stringSize;
    if (oldSize != 0)
        dyStringAppend(dy, ", ");
    dyStringPrintf(dy, "%s (%d)", hel->name, ptToInt(hel->val));
    if (dy->stringSize >= maxSize)
        {
	dy->string[oldSize] = 0;
	dy->stringSize = oldSize;
	dyStringAppend(dy, "...");
	break;
	}
    }
hashElFreeList(&helList);
return dy;
}

static long long sumCounts(struct hash *hash)
/* Figuring hash is integer valued, return sum of all vals in hash */
{
long long total = 0;
struct hashEl *hel, *helList = hashElListHash(hash);
for (hel = helList; hel != NULL; hel = hel->next)
    {
    int val = ptToInt(hel->val);
    total += val;
    }
hashElFreeList(&helList);
return total;
}

static int matchCount = 0;
static boolean doSelect = FALSE;

static void rMatchesToRa(struct tagStorm *tags, struct tagStanza *list, 
    struct rqlStatement *rql, struct lm *lm)
/* Recursively traverse stanzas on list outputting matching stanzas as ra. */
{
struct tagStanza *stanza;
for (stanza = list; stanza != NULL; stanza = stanza->next)
    {
    if (rql->limit < 0 || rql->limit > matchCount)
	{
	if (stanza->children)
	    rMatchesToRa(tags, stanza->children, rql, lm);
	else    /* Just apply query to leaves */
	    {
	    if (cdwRqlStatementMatch(rql, stanza, lm))
		{
		++matchCount;
		if (doSelect)
		    {
		    struct slName *field;
		    for (field = rql->fieldList; field != NULL; field = field->next)
			{
			char *val = tagFindVal(stanza, field->name);
			if (val != NULL)
			    printf("%s\t%s\n", field->name, val);
			}
		    printf("\n");
		    }
		}
	    }
	}
    }
}

void showMatchingAsRa(char *rqlQuery, int limit, struct tagStorm *tags)
/* Show stanzas that match query */
{
struct dyString *dy = dyStringCreate("%s", rqlQuery);
int maxLimit = 10000;
if (limit > maxLimit)
    limit = maxLimit;
struct rqlStatement *rql = rqlStatementParseString(dy->string);

/* Get list of all tag types in tree and use it to expand wildcards in the query
 * field list. */
struct slName *allFieldList = tagStormFieldList(tags);
slSort(&allFieldList, slNameCmpCase);
rql->fieldList = wildExpandList(allFieldList, rql->fieldList, TRUE);

/* Traverse tag tree outputting when rql statement matches in select case, just
 * updateing count in count case. */
doSelect = sameWord(rql->command, "select");
if (doSelect)
    rql->limit = limit;
struct lm *lm = lmInit(0);
rMatchesToRa(tags, tags->forest, rql, lm);
if (sameWord(rql->command, "count"))
    printf("%d\n", matchCount);

}

int labCount(struct tagStorm *tags)
/* Return number of different labs in tags */
{
struct hash *hash = tagStormCountTagVals(tags, "lab");
int count = hash->elCount;
hashFree(&hash);
return count;
}


void wrapFileName(struct fieldedTable *table, struct fieldedRow *row, 
    char *field, char *val, void *context)
/* Write out wrapper that links us to something nice */
{
printf("<A HREF=\"../cgi-bin/cdwWebBrowse?cdwCommand=oneFile&cdwFileTag=%s&cdwFileVal=%s&%s\">",
    field, val, cartSidUrlString(cart));
printf("%s</A>", val);
}

void wrapTagField(struct fieldedTable *table, struct fieldedRow *row, 
    char *field, char *val, void *context)
/* Write out wrapper that links us to something nice */
{
printf("<A HREF=\"../cgi-bin/cdwWebBrowse?cdwCommand=oneTag&cdwTagName=%s&%s\">",
    val, cartSidUrlString(cart));
printf("%s</A>", val);
}

void wrapTagValueInFiles(struct fieldedTable *table, struct fieldedRow *row, 
    char *field, char *val, void *context)
/* Write out wrapper that links us to something nice */
{
printf("<A HREF=\"../cgi-bin/cdwWebBrowse?cdwCommand=browseFiles&%s&",
    cartSidUrlString(cart));
char query[2*PATH_LEN];
safef(query, sizeof(query), "%s = '%s'", field, val);
char *escapedQuery = cgiEncode(query);
printf("%s=%s", "cdwFile_filter", escapedQuery);
freez(&escapedQuery);
printf("\">%s</A>", val);
}

static char *mustFindFieldInRow(char *field, struct slName *fieldList, char **row)
/* Assuming field is in list, which is ordered same as row, return row cell
 * corrsepondint to field */
{
int fieldIx = 0;
struct slName *el;
for (el = fieldList; el != NULL; el = el->next)
    {
    if (sameString(el->name, field))
        {
	return row[fieldIx];
	}
    ++fieldIx;
    }
errAbort("Couldn't find field %s in row", field);
return NULL;
}

void doOneTag(struct sqlConnection *conn)
/* Put up information on one tag */
{
char *tag = cartString(cart, "cdwTagName");
char query[512];
sqlSafef(query, sizeof(query), "select count(*) from cdwFileTags where %s is not null", tag);
int taggedFileCount = sqlQuickNum(conn, query);
sqlSafef(query, sizeof(query), "select count(distinct(%s)) from cdwFileTags where %s is not null", 
    tag, tag);
int distinctCount = sqlQuickNum(conn, query);
printf("The <B>%s</B> tag has %d distinct values and is used on %d files. ", 
    tag, distinctCount, taggedFileCount);
int maxLimit = 100;
int limit = min(maxLimit, taggedFileCount);
if (distinctCount > maxLimit)
    printf("The %d most popular values of %s are:<BR>\n", maxLimit, tag);
else
    printf("The values for %s are:<BR>\n", tag);

sqlSafef(query, sizeof(query), 
    "select count(%s) ct,%s from cdwFileTags group by %s order by ct desc "
    " limit %d ", 
    tag, tag, tag, limit);
struct sqlResult *sr = sqlGetResult(conn, query);
char *labels[] = {"files", tag};
int fieldCount = ArraySize(labels);
struct fieldedTable *table = fieldedTableNew("Tag Values", labels, fieldCount);
char **row;
while ((row = sqlNextRow(sr)) != NULL)
    {
    if (row[1] != NULL)
	fieldedTableAdd(table, row, fieldCount, 0);
    }

char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?cdwCommand=oneTag&cdwTagName=%s&%s",
    tag, cartSidUrlString(cart) );
struct hash *outputWrappers = hashNew(0);
hashAdd(outputWrappers, tag, wrapTagValueInFiles);
webSortableFieldedTable(cart, table, returnUrl, "cdwOneTag", 0, outputWrappers, NULL);
fieldedTableFree(&table);
}

void doOneFile(struct sqlConnection *conn)
/* Put up a page with info on one file */
{
char *idTag = cartUsualString(cart, "cdwFileTag", "accession");
char *idVal = cartString(cart, "cdwFileVal");
char query[512];
sqlSafef(query, sizeof(query), "select * from cdwFileTags where %s='%s'", idTag, idVal);
struct sqlResult *sr = sqlGetResult(conn, query);
struct slName *el, *list = sqlResultFieldList(sr);
char **row;
while ((row = sqlNextRow(sr)) != NULL)
    {
    char *fileName = mustFindFieldInRow("file_name", list, row);
    char *fileSize = mustFindFieldInRow("file_size", list, row);
    char *format = mustFindFieldInRow("format", list, row);
    long long size = atoll(fileSize);
    printf("Tags associated with %s a %s format file of size ", fileName, format);
    printLongWithCommas(stdout, size);
    printf("<BR>\n");


    static char *outputFields[] = {"tag", "value"};
    struct fieldedTable *table = fieldedTableNew("File Tags", outputFields,ArraySize(outputFields));
    int fieldIx = 0;
    for (el = list; el != NULL; el = el->next)
        {
	char *outRow[2];
	char *val = row[fieldIx];
	if (val != NULL)
	    {
	    outRow[0] = el->name;
	    outRow[1] = row[fieldIx];
	    fieldedTableAdd(table, outRow, 2, fieldIx);
	    }
	++fieldIx;
	}
    char returnUrl[PATH_LEN*2];
    safef(returnUrl, sizeof(returnUrl), 
	"../cgi-bin/cdwWebBrowse?cdwCommand=oneFile&cdwFileTag=%s&cdwFileVal=%s&%s",
	idTag, idVal, cartSidUrlString(cart) );
    struct hash *outputWrappers = hashNew(0);
    hashAdd(outputWrappers, "tag", wrapTagField);
    webSortableFieldedTable(cart, table, returnUrl, "cdwOneFile", 0, outputWrappers, NULL);
    fieldedTableFree(&table);
    }
}

struct dyString *customTextForFile(struct sqlConnection *conn, struct cdwTrackViz *viz)
/* Create custom track text */
{
struct dyString *dy = dyStringNew(0);
dyStringPrintf(dy, "track name=\"%s\" ", viz->shortLabel);
dyStringPrintf(dy, "description=\"%s\" ", viz->longLabel);
char *host = hHttpHost();
dyStringPrintf(dy, "bigDataUrl=http://%s/cdw/%s type=%s", host, viz->bigDataFile, viz->type);
return dy;
}

struct cdwTrackViz *cdwTrackVizFromFileId(struct sqlConnection *conn, long long fileId)
/* Return cdwTrackViz if any associated with file ID */
{
char query[256];
sqlSafef(query, sizeof(query), "select * from cdwTrackViz where fileId=%lld", fileId);
return cdwTrackVizLoadByQuery(conn, query);
}


boolean wrapTrackVis(struct sqlConnection *conn, struct cdwValidFile *vf, char *unwrapped)
/* Attempt to wrap genome browser link around unwrapped text.  Link goes to file in vf. */
{
if (vf == NULL)
    return FALSE;
struct cdwTrackViz *viz = cdwTrackVizFromFileId(conn, vf->fileId);
if (viz == NULL)
    return FALSE;
struct dyString *track = customTextForFile(conn, viz);
char *encoded = cgiEncode(track->string);
printf("<A HREF=\"../cgi-bin/hgTracks");
printf("?%s", cartSidUrlString(cart));
printf("&db=%s", vf->ucscDb);
printf("&hgt.customText=");
printf("%s", encoded);
printf("\">");	       // Finish HREF quote and A tag
printf("%s</A>", unwrapped);
freez(&encoded);
dyStringFree(&track);
return TRUE;
}


#ifdef UNUSED
void wrapTrackAccession(struct fieldedTable *table, struct fieldedRow *row, char *tag, char *val, 
    void *context)
/* Write out wrapper to link us to genome browser */
{
struct sqlConnection *conn = context;
struct cdwValidFile *vf = cdwValidFileFromLicensePlate(conn, val);
if (!wrapTrackVis(conn, vf, val))
    {
    printf("%s (needs viz)", val);
    }
}
#endif /* UNUSED */

void wrapTrackNearFileName(struct fieldedTable *table, struct fieldedRow *row, char *tag, char *val,
    void *context)
/* Construct wrapper to UCSC if row actually is a track */
{
struct sqlConnection *conn = context;
int fileNameIx = stringArrayIx("file_name", table->fields, table->fieldCount);
if (fileNameIx >= 0)
    {
    char *fileName = row->row[fileNameIx];
    char acc[FILENAME_LEN];
    safef(acc, sizeof(acc), "%s", fileName);
    char *dot = strchr(acc, '.');
    if (dot != NULL)
        *dot = 0;
    struct cdwValidFile *vf = cdwValidFileFromLicensePlate(conn, acc);
    if (!wrapTrackVis(conn, vf, val))
	printf("%s", val);
    }
else
    printf("%s", val);
}

void wrapTrackNearAccession(struct fieldedTable *table, struct fieldedRow *row, 
    char *tag, char *val, void *context)
/* Construct wrapper that can link to Genome Browser if any field in table
 * is an accession */
{
struct sqlConnection *conn = context;
int accIx = stringArrayIx("accession", table->fields, table->fieldCount);
if (accIx >= 0)
    {
    char *acc = row->row[accIx];
    if (acc != NULL)
	{
	struct cdwValidFile *vf = cdwValidFileFromLicensePlate(conn, acc);
	if (!wrapTrackVis(conn, vf, val))
	    printf("%s", val);
	}
    }
else
    printf("%s", val);
}

void wrapMetaNearAccession(struct fieldedTable *table, struct fieldedRow *row, 
    char *field, char *val, void *context)
/* Write out wrapper on a column that looks for accession in same table and uses
 * that to link us to oneFile display. */
{
struct sqlConnection *conn = context;
int accIx = stringArrayIx("accession", table->fields, table->fieldCount);
boolean wrapped = FALSE;
if (accIx >= 0)
    {
    char *acc = row->row[accIx];
    if (acc != NULL)
        {
	struct cdwValidFile *vf = cdwValidFileFromLicensePlate(conn, acc);
	if (vf != NULL)
	    {
	    wrapped = TRUE;
	    printf("<A HREF=\"../cgi-bin/cdwWebBrowse?cdwCommand=oneFile&cdwFileTag=accession&cdwFileVal=%s&%s\">",
		acc, cartSidUrlString(cart));
	    printf("%s</A>", val);
	    }
	}
    }

if (!wrapped)
    printf("%s", val);
}


void doBrowseFiles(struct sqlConnection *conn)
/* Print list of files */
{
printf("<FORM ACTION=\"../cgi-bin/cdwWebBrowse\" METHOD=GET>\n");
cartSaveSession(cart);
cgiMakeHiddenVar("cdwCommand", "browseFiles");

printf("<B>Files</B> - sort and select lists of files. Click on file's name to see full metadata.");
printf(" Links in ucsc_db go to the Genome Browser. &nbsp;&nbsp;&nbsp;&nbsp;");

/* Put up big filtered table of files */
char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?cdwCommand=browseFiles&%s",
    cartSidUrlString(cart) );
char *where = cartUsualString(cart, "cdwFile_filter", "");
if (!isEmpty(where))
    {
    printf("<BR>Restricting files to where %s. ", where);
    }
struct hash *wrappers = hashNew(0);
hashAdd(wrappers, "file_name", wrapFileName);
hashAdd(wrappers, "ucsc_db", wrapTrackNearFileName);
webFilteredSqlTable(cart, conn, 
    "file_name,file_size,ucsc_db,lab,assay,data_set_id,output,format,read_size,item_count,"
         "body_part",
    "cdwFileTags", where, 
    returnUrl, "cdwBrowseFiles",
    18, wrappers, conn, TRUE, "files", 100);

printf("</FORM>\n");
}

void doBrowseTracks(struct sqlConnection *conn)
/* Print list of files */
{
printf("<FORM ACTION=\"../cgi-bin/cdwWebBrowse\" METHOD=GET>\n");
cartSaveSession(cart);
cgiMakeHiddenVar("cdwCommand", "browseTracks");

printf("<B>Tracks</B> - Click on ucsc_db to open Genome Browser. ");
printf("The accession link shows more metadata. &nbsp;&nbsp;&nbsp;&nbsp;");
char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?cdwCommand=browseTracks&%s",
    cartSidUrlString(cart) );
char *where = "fileId=file_id and format in ('bam','bigBed', 'bigWig', 'vcf', 'narrowPeak', 'broadPeak')";
struct hash *wrappers = hashNew(0);
hashAdd(wrappers, "accession", wrapMetaNearAccession);
hashAdd(wrappers, "ucsc_db", wrapTrackNearAccession);
webFilteredSqlTable(cart, conn, 
    "ucsc_db,chrom,accession,format,file_size,lab,assay,data_set_id,output,"
    "enriched_in,body_part,submit_file_name",
    "cdwFileTags,cdwTrackViz", where, 
    returnUrl, "cdwBrowseTracks", 
    22, wrappers, conn, TRUE, "tracks", 100);
printf("</FORM>\n");
}


void doBrowsePopularTags(struct sqlConnection *conn, char *tag)
/* Print list of most popular tags of type */
{
struct tagStorm *tags = cdwTagStorm(conn);
struct hash *hash = tagStormCountTagVals(tags, tag);
printf("%s tag values ordered by usage\n", tag);
struct hashEl *hel, *helList = hashElListHash(hash);
slSort(&helList, hashElCmpIntValDesc);
webPrintLinkTableStart();
webPrintLabelCell("#");
webPrintLabelCell(tag);
webPrintLabelCell("matching files");
int valIx = 0, maxValIx = 100;
for (hel = helList; hel != NULL && ++valIx <= maxValIx; hel = hel->next)
    {
    printf("<TR>\n");
    webPrintIntCell(valIx);
    webPrintLinkCell(hel->name);
    webPrintIntCell(ptToInt(hel->val));
    printf("</TR>\n");
    }
webPrintLinkTableEnd();
}

void doBrowseFormat(struct sqlConnection *conn)
/* Browse through available formats */
{
static char *labels[] = {"count", "format", "description"};
int fieldCount = ArraySize(labels);
char *row[fieldCount];
struct fieldedTable *table = fieldedTableNew("Data formats", labels, fieldCount);
struct slPair *format, *formatList = cdwFormatList();

for (format = formatList; format != NULL; format = format->next)
    {
    char countString[16];
    char query[256];
    sqlSafef(query, sizeof(query), "select count(*) from cdwFileTags where format='%s'", 
	format->name);
    sqlQuickQuery(conn, query, countString, sizeof(countString));
    row[0] = countString;
    row[1] = format->name;
    row[2] = format->val;
    fieldedTableAdd(table, row, fieldCount, 0);
    }
char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?cdwCommand=browseFormats&%s",
    cartSidUrlString(cart) );
webSortableFieldedTable(cart, table, returnUrl, "cdwFormats", 0, NULL, NULL);
fieldedTableFree(&table);
}

void doBrowseLab(struct sqlConnection *conn)
/* Put up information on labs */
{
static char *labels[] = {"name", "files", "PI", "institution", "web page"};
int fieldCount = ArraySize(labels);
char *row[fieldCount];
struct fieldedTable *table = fieldedTableNew("Data contributing labs", labels, fieldCount);

printf("Here is a table of labs that have contributed data<BR>\n");
char query[256];
sqlSafef(query, sizeof(query), "select * from cdwLab");
struct cdwLab *lab, *labList = cdwLabLoadByQuery(conn, query);
int i = 0;
for (lab = labList; lab != NULL; lab = lab->next)
    {
    row[0] = lab->name;
    char countString[16];
    sqlSafef(query, sizeof(query), "select count(*) from cdwFileTags where lab='%s'", lab->name);
    sqlQuickQuery(conn, query, countString, sizeof(countString));
    row[1] = countString;
    row[2] = lab->pi;
    row[3] = lab->institution;
    row[4] = lab->url;
    fieldedTableAdd(table, row, fieldCount, ++i);
    }
char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?cdwCommand=browseLabs&%s",
    cartSidUrlString(cart) );
webSortableFieldedTable(cart, table, returnUrl, "cdwLab", 0, NULL, NULL);
fieldedTableFree(&table);
}

void doQuery(struct sqlConnection *conn)
/* Print up query page */
{
/* Do stuff that keeps us here after a routine submit */
printf("<FORM ACTION=\"../cgi-bin/cdwWebBrowse\" METHOD=GET>\n");
cartSaveSession(cart);
cgiMakeHiddenVar("cdwCommand", "query");

/* Get values from text inputs and make up an RQL query string out of fields, where, limit
 * clauses */

/* Fields clause */
char *fieldsVar = "cdwQueryFields";
char *fields = cartUsualString(cart, fieldsVar, "*");
struct dyString *rqlQuery = dyStringCreate("select %s from files ", fields);

/* Where clause */
char *whereVar = "cdwQueryWhere";
char *where = cartUsualString(cart, whereVar, "");
dyStringPrintf(rqlQuery, "where accession");
if (!isEmpty(where))
    dyStringPrintf(rqlQuery, " and (%s)", where);

/* Limit clause */
char *limitVar = "cdwQueryLimit";
int limit = cartUsualInt(cart, limitVar, 10);

struct tagStorm *tags = cdwTagStorm(conn);
struct slRef *matchList = tagStanzasMatchingQuery(tags, rqlQuery->string);
int matchCount = slCount(matchList);
printf("Enter a SQL-like query below. ");
printf("In the box after 'select' you can put in a list of tag names including wildcards. ");
printf("In the box after 'where' you can put in filters based on boolean operations. <BR>");
/** Write out select [  ] from files whre [  ] limit [ ] <submit> **/
printf("select ");
cgiMakeTextVar(fieldsVar, fields, 40);
printf("from files ");
printf("where ");
cgiMakeTextVar(whereVar, where, 40);
printf(" limit ");
cgiMakeIntVar(limitVar, limit, 7);
cgiMakeSubmitButton();

printf("<PRE><TT>");
printLongWithCommas(stdout, matchCount);
printf(" files match\n\n");
showMatchingAsRa(rqlQuery->string, limit, tags);
printf("</TT></PRE>\n");
printf("</FORM>\n");
}

void tagSummaryRow(struct fieldedTable *table, struct tagStorm *tags, char *tag)
/* Print out row in a high level tag counting table */
{
/* Do analysis hash */
struct hash *hash = tagStormCountTagVals(tags, tag);

/* Convert count of distinct values to string */
int valCount = hash->elCount;
char valCountString[32];
safef(valCountString, sizeof(valCountString), "%d", valCount);

/* Convert count of files using tag to string */
int fileCount = sumCounts(hash);
char fileCountString[32];
safef(fileCountString, sizeof(fileCountString), "%d", fileCount);

struct dyString *dy = printPopularTags(hash, 120);

/* Add data to fielded table */
char *row[4];
row[0] = tag;
row[1] = valCountString;
row[2] = dy->string;
row[3] = fileCountString;
fieldedTableAdd(table, row, ArraySize(row), 0);

/* Clean up */
dyStringFree(&dy);
hashFree(&hash);
}

char *tagPopularityFields[] = { "tag name", "vals", "popular values (files)...", "files",};

void doHome(struct sqlConnection *conn)
/* Put up home/summary page */
{
struct tagStorm *tags = cdwTagStorm(conn);

/* Print sentence with summary of bytes, files, and labs */
char query[256];
printf("The CIRM Stem Cell Hub contains ");
sqlSafef(query, sizeof(query),
    "select sum(size) from cdwFile,cdwValidFile where cdwFile.id=cdwValidFile.id");
long long totalBytes = sqlQuickLongLong(conn, query);
printLongWithCommas(stdout, totalBytes);
printf(" bytes of data in ");
sqlSafef(query, sizeof(query), "select count(*) from cdwValidFile");
long long fileCount = sqlQuickLongLong(conn, query);
printLongWithCommas(stdout, fileCount);
printf(" files");
printf(" from %d labs.<BR>\n", labCount(tags));
printf("Try using the browse menu on files, tracks or tags. ");
printf("The query link allows simple SQL-like queries of the metadata.");
printf("<BR><BR>\n");

/* Print out high level tags table */
static char *highLevelTags[] = 
    {"data_set_id", "lab", "assay", "format", "read_size",
    "body_part", "submit_dir", "lab_quake_markers", "species"};

struct fieldedTable *table = fieldedTableNew("Important tags", tagPopularityFields, 
    ArraySize(tagPopularityFields));
int i;
for (i=0; i<ArraySize(highLevelTags); ++i)
    tagSummaryRow(table, tags, highLevelTags[i]);
char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?%s", cartSidUrlString(cart) );
webSortableFieldedTable(cart, table, returnUrl, "cdwHome", 0, NULL, NULL);

printf("This table is a summary of important metadata tags including the tag name, the number of ");
printf("values and the most popular values of the tag, and the number of files marked with ");
printf("the tag.");

printf("<BR>\n");

tagStormFree(&tags);
}

void doBrowseTags(struct sqlConnection *conn)
/* Put up browse tags page */
{
struct tagStorm *tags = cdwTagStorm(conn);
struct slName *tag, *tagList = tagStormFieldList(tags);
slSort(&tagList, slNameCmp);
printf("This is a list of all tags and their most popular values.");
struct fieldedTable *table = fieldedTableNew("Important tags", tagPopularityFields, 
    ArraySize(tagPopularityFields));
for (tag = tagList; tag != NULL; tag = tag->next)
    tagSummaryRow(table, tags, tag->name);
char returnUrl[PATH_LEN*2];
safef(returnUrl, sizeof(returnUrl), "../cgi-bin/cdwWebBrowse?cdwCommand=browseTags&%s",
    cartSidUrlString(cart) );
struct hash *outputWrappers = hashNew(0);
    hashAdd(outputWrappers, "tag name", wrapTagField);
webSortableFieldedTable(cart, table, returnUrl, "cdwBrowseTags", 0, outputWrappers, NULL);
tagStormFree(&tags);
}

void doHelp(struct sqlConnection *con)
/* Put up help page */
{
printf("This being a prototype, there's not much help available.  Try clicking and hovering over the Browse link on the top bar to expose a menu. The trickiest part of the system is the query link.");
printf("The query link has you type in a SQL-like query. ");
printf("Try 'select * from files where accession' to get all metadata tags ");
printf("from files that have passed basic format validations. Instead of '*' you ");
printf("could use a comma separated list of tag names. ");
printf("Instead of 'accession' you could put in a boolean expression involving field names and ");
printf("constants. String constants need to be surrounded by quotes - either single or double.");
printf("<BR><BR>");
}


void dispatch(struct sqlConnection *conn)
/* Dispatch page after to routine depending on cdwCommand variable */
{
char *command = cartOptionalString(cart, "cdwCommand");
if (command == NULL)
    {
    doHome(conn);
    }
else if (sameString(command, "query"))
    {
    doQuery(conn);
    }
else if (sameString(command, "browseFiles"))
    {
    doBrowseFiles(conn);
    }
else if (sameString(command, "browseTracks"))
    {
    doBrowseTracks(conn);
    }
else if (sameString(command, "browseTags"))
    {
    doBrowseTags(conn);
    }
else if (sameString(command, "browseLabs"))
    {
    doBrowseLab(conn);
    }
else if (sameString(command, "browseDataSets"))
    {
    doBrowsePopularTags(conn, "data_set_id");
    }
else if (sameString(command, "browseFormats"))
    {
    doBrowseFormat(conn);
    }
else if (sameString(command, "oneFile"))
    {
    doOneFile(conn);
    }
else if (sameString(command, "oneTag"))
    {
    doOneTag(conn);
    }
else if (sameString(command, "help"))
    {
    doHelp(conn);
    }
else
    {
    printf("unrecognized command %s<BR>\n", command);
    }
}

void doMiddle()
/* Menu bar has been drawn.  We are in the middle of first section. */
{
struct sqlConnection *conn = sqlConnect(cdwDatabase);
dispatch(conn);
sqlDisconnect(&conn);
}

static char *localMenuBar()
/* Return menu bar string */
{
// menu bar html is in a stringified .h file
char *rawHtml = 
#include "cdwNavBar.h"
   ;

char uiVars[128];
safef(uiVars, sizeof(uiVars), "%s=%s", cartSessionVarName(), cartSessionId(cart));
return menuBarAddUiVars(rawHtml, "/cgi-bin/cdw", uiVars);
}

void localWebStartWrapper(char *titleString)
/* Output a HTML header with the given title.  Start table layout.  Draw menu bar. */
{
/* Do html header. We do this a little differently than web.c routines, mostly
 * in that we are strict rather than transitional HTML 4.01 */
    {
    puts("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
             "\"http://www.w3.org/TR/html4/strict.dtd\">");
    puts("<HTML><HEAD>\n");
    webPragmasEtc();
    printf("<TITLE>%s</TITLE>\n", titleString);
    webIncludeResourceFile("HGStyle.css");
    jsIncludeFile("jquery.js", NULL);
    jsIncludeFile("jquery.plugins.js", NULL);
    webIncludeResourceFile("nice_menu.css");
    printf("</HEAD>\n");
    printBodyTag(stdout);
    }

webStartSectionTables();    // Start table layout code
puts(localMenuBar());	    // Menu bar after tables open but before first section
webFirstSection(titleString);	// Open first section
webPushErrHandlers();	    // Now can do improved error handler
}


void localWebWrap(struct cart *theCart)
/* We got the http stuff handled, and a cart.  Now wrap a web page around it. */
{
cart = theCart;
localWebStartWrapper("CIRM Stem Cell Hub Browser V0.27");
pushWarnHandler(htmlVaWarn);
doMiddle();
webEndSectionTables();
printf("</BODY></HTML>\n");
}


char *excludeVars[] = {"cdwCommand", "submit", NULL};

int main(int argc, char *argv[])
/* Process command line. */
{
boolean isFromWeb = cgiIsOnWeb();
if (!isFromWeb && !cgiSpoof(&argc, argv))
    usage();
dnaUtilOpen();
oldVars = hashNew(0);
cartEmptyShell(localWebWrap, hUserCookie(), excludeVars, oldVars);
return 0;
}