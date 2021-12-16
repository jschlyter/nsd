/*
 * ixfrcreate.c -- generating IXFR differences from zone files.
 *
 * Copyright (c) 2021, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include "config.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "ixfrcreate.h"
#include "namedb.h"
#include "ixfr.h"

/* spool a uint16_t to file */
static int spool_u16(FILE* out, uint16_t val)
{
	if(!fwrite(&val, sizeof(val), 1, out)) {
		return 0;
	}
	return 1;
}

/* spool a uint32_t to file */
static int spool_u32(FILE* out, uint32_t val)
{
	if(!fwrite(&val, sizeof(val), 1, out)) {
		return 0;
	}
	return 1;
}

/* spool dname to file */
static int spool_dname(FILE* out, dname_type* dname)
{
	uint16_t namelen = dname->name_size;
	if(!fwrite(&namelen, sizeof(namelen), 1, out)) {
		return 0;
	}
	if(!fwrite(dname_name(dname), namelen, 1, out)) {
		return 0;
	}
	return 1;
}

/* calculate the rdatalen of an RR */
static size_t rr_rdatalen_uncompressed(rr_type* rr)
{
	int i;
	size_t rdlen_uncompressed = 0;
	for(i=0; i<rr->rdata_count; i++) {
		if(rdata_atom_is_domain(rr->type, i)) {
			rdlen_uncompressed += domain_dname(rr->rdatas[i].domain)
				->name_size;
		} else {
			rdlen_uncompressed += rr->rdatas[i].data[0];
		}
	}
	return rdlen_uncompressed;
}

/* spool the data for one rr into the file */
static int spool_rr_data(FILE* out, rr_type* rr)
{
	int i;
	uint16_t rdlen;
	if(!spool_u32(out, rr->ttl))
		return 0;
	rdlen = rr_rdatalen_uncompressed(rr);
	if(!spool_u16(out, rdlen))
		return 0;
	for(i=0; i<rr->rdata_count; i++) {
		if(rdata_atom_is_domain(rr->type, i)) {
			if(!fwrite(dname_name(domain_dname(
				rr->rdatas[i].domain)), domain_dname(
				rr->rdatas[i].domain)->name_size, 1, out))
				return 0;
		} else {
			if(!fwrite(&rr->rdatas[i].data[1],
				rr->rdatas[i].data[0], 1, out))
				return 0;
		}
	}
	return 1;
}

/* spool one rrset to file */
static int spool_rrset(FILE* out, rrset_type* rrset)
{
	int i;
	if(rrset->rr_count == 0)
		return 1;
	if(!spool_u16(out, rrset->rrs[0].type))
		return 0;
	if(!spool_u16(out, rrset->rrs[0].klass))
		return 0;
	if(!spool_u16(out, rrset->rr_count))
		return 0;
	for(i=0; i<rrset->rr_count; i++) {
		if(!spool_rr_data(out, &rrset->rrs[i]))
			return 0;
	}
	return 1;
}

/* spool rrsets to file */
static int spool_rrsets(FILE* out, rrset_type* rrsets, struct zone* zone)
{
	rrset_type* s;
	for(s=rrsets; s; s=s->next) {
		if(s->zone != zone)
			continue;
		if(!spool_rrset(out, s)) {
			return 0;
		}
	}
	return 1;
}

/* count number of rrsets for a domain */
static size_t domain_count_rrsets(domain_type* domain, zone_type* zone)
{
	rrset_type* s;
	size_t count = 0;
	for(s=domain->rrsets; s; s=s->next) {
		if(s->zone == zone)
			count++;
	}
	return count;
}

/* spool the domain names to file, each one in turn. end with enddelimiter */
static int spool_domains(FILE* out, struct zone* zone)
{
	domain_type* domain;
	for(domain = zone->apex; domain && domain_is_subdomain(domain,
		zone->apex); domain = domain_next(domain)) {
		uint32_t count = domain_count_rrsets(domain, zone);
		if(count == 0)
			continue;
		/* write the name */
		if(!spool_dname(out, domain_dname(domain)))
			return 0;
		if(!spool_u32(out, count))
			return 0;
		/* write the rrsets */
		if(!spool_rrsets(out, domain->rrsets, zone))
			return 0;
	}
	/* the end delimiter is a 0 length. domain names are not zero length */
	if(!spool_u16(out, 0))
		return 0;
	return 1;
}

/* spool the namedb zone to the file. print error on failure. */
static int spool_zone_to_file(struct zone* zone, char* file_name,
	uint32_t serial)
{
	FILE* out;
	out = fopen(file_name, "w");
	if(!out) {
		log_msg(LOG_ERR, "could not open %s for writing: %s",
			file_name, strerror(errno));
		return 0;
	}
	if(!spool_dname(out, domain_dname(zone->apex))) {
		log_msg(LOG_ERR, "could not write %s: %s",
			file_name, strerror(errno));
		return 0;
	}
	if(!spool_u32(out, serial)) {
		log_msg(LOG_ERR, "could not write %s: %s",
			file_name, strerror(errno));
		return 0;
	}
	if(!spool_domains(out, zone)) {
		log_msg(LOG_ERR, "could not write %s: %s",
			file_name, strerror(errno));
		return 0;
	}
	fclose(out);
	return 1;
}

/* create ixfr spool file name */
static int create_ixfr_spool_name(struct ixfr_create* ixfrcr, char* zfile)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s.spoolzone.%u", zfile,
		(unsigned)getpid());
	ixfrcr->file_name = strdup(buf);
	if(!ixfrcr->file_name)
		return 0;
	return 1;
}

/* start ixfr creation */
struct ixfr_create* ixfr_create_start(struct zone* zone, char* zfile)
{
	struct ixfr_create* ixfrcr = (struct ixfr_create*)calloc(1,
		sizeof(*ixfrcr));
	if(!ixfrcr) {
		log_msg(LOG_ERR, "malloc failure");
		return NULL;
	}
	ixfrcr->zone_name_len = domain_dname(zone->apex)->name_size;
	ixfrcr->zone_name = (uint8_t*)malloc(ixfrcr->zone_name_len);
	if(!ixfrcr->zone_name) {
		free(ixfrcr);
		log_msg(LOG_ERR, "malloc failure");
		return NULL;
	}
	memmove(ixfrcr->zone_name, dname_name(domain_dname(zone->apex)),
		ixfrcr->zone_name_len);

	if(!create_ixfr_spool_name(ixfrcr, zfile)) {
		ixfr_create_free(ixfrcr);
		log_msg(LOG_ERR, "malloc failure");
		return NULL;
	}
	ixfrcr->old_serial = zone_get_current_serial(zone);
	if(!spool_zone_to_file(zone, ixfrcr->file_name, ixfrcr->old_serial)) {
		ixfr_create_free(ixfrcr);
		return NULL;
	}
	return ixfrcr;
}

/* free ixfr create */
void ixfr_create_free(struct ixfr_create* ixfrcr)
{
	if(!ixfrcr)
		return;
	free(ixfrcr->file_name);
	free(ixfrcr->zone_name);
	free(ixfrcr);
}

/* read uint16_t from spool */
static int read_spool_u16(FILE* spool, uint16_t* val)
{
	if(!fread(val, sizeof(*val), 1, spool))
		return 0;
	return 1;
}

/* read uint32_t from spool */
static int read_spool_u32(FILE* spool, uint32_t* val)
{
	if(!fread(val, sizeof(*val), 1, spool))
		return 0;
	return 1;
}

/* read dname from spool */
static int read_spool_dname(FILE* spool, uint8_t* buf, size_t buflen,
	size_t* dname_len)
{
	uint16_t len;
	if(!fread(&len, sizeof(len), 1, spool))
		return 0;
	if(len > buflen) {
		log_msg(LOG_ERR, "dname too long");
		return 0;
	}
	if(len > 0) {
		if(!fread(buf, len, 1, spool))
			return 0;
	}
	*dname_len = len;
	return 1;
}

/* read and check the spool file header */
static int read_spool_header(FILE* spool, struct ixfr_create* ixfrcr)
{
	uint8_t dname[MAXDOMAINLEN+1];
	size_t dname_len;
	uint32_t serial;
	/* read apex */
	if(!read_spool_dname(spool, dname, sizeof(dname), &dname_len)) {
		log_msg(LOG_ERR, "error reading file %s: %s",
			ixfrcr->file_name, strerror(errno));
		return 0;
	}
	/* read serial */
	if(!read_spool_u32(spool, &serial)) {
		log_msg(LOG_ERR, "error reading file %s: %s",
			ixfrcr->file_name, strerror(errno));
		return 0;
	}

	/* check */
	if(ixfrcr->zone_name_len != dname_len ||
		memcmp(ixfrcr->zone_name, dname, ixfrcr->zone_name_len) != 0) {
		log_msg(LOG_ERR, "error file %s does not contain the correct zone apex",
			ixfrcr->file_name);
		return 0;
	}
	if(ixfrcr->old_serial != serial) {
		log_msg(LOG_ERR, "error file %s does not contain the correct zone serial",
			ixfrcr->file_name);
		return 0;
	}
	return 1;
}

/* see if rdata matches, true if equal */
static int rdata_match(struct rr* rr, uint8_t* rdata, uint16_t rdlen)
{
	size_t rdpos = 0;
	int i;
	for(i=0; i<rr->rdata_count; i++) {
		if(rdata_atom_is_domain(rr->type, i)) {
			if(rdpos + domain_dname(rr->rdatas[i].domain)->name_size
				> rdlen)
				return 0;
			if(memcmp(rdata+rdpos,
				dname_name(domain_dname(rr->rdatas[i].domain)),
				domain_dname(rr->rdatas[i].domain)->name_size)
				!= 0)
				return 0;
			rdpos += domain_dname(rr->rdatas[i].domain)->name_size;
		} else {
			if(rdpos + rr->rdatas[i].data[0] > rdlen)
				return 0;
			if(memcmp(rdata+rdpos, &rr->rdatas[i].data[1],
				rr->rdatas[i].data[0]) != 0)
				return 0;
			rdpos += rr->rdatas[i].data[0];
		}
	}
	if(rdpos != rdlen)
		return 0;
	return 1;
}

/* find an rdata in an rrset, true if found and sets index found */
static int rrset_find_rdata(struct rrset* rrset, uint32_t ttl, uint8_t* rdata,
	uint16_t rdlen, uint16_t* index)
{
	int i;
	for(i=0; i<rrset->rr_count; i++) {
		if(rrset->rrs[i].ttl != ttl)
			continue;
		if(rdata_match(&rrset->rrs[i], rdata, rdlen)) {
			*index = i;
			return 1;
		}
	}
	return 0;
}

/* spool read an rrset, it is a deleted RRset */
static int process_diff_rrset(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, struct domain* domain,
	uint16_t tp, uint16_t kl, uint16_t rrcount, struct rrset* rrset)
{
	/* read RRs from file and see if they are added, deleted or in both */
	uint8_t buf[MAX_RDLENGTH];
	uint16_t marked[65536];
	size_t marked_num = 0;
	int i;
	for(i=0; i<rrcount; i++) {
		uint16_t rdlen, index;
		uint32_t ttl;
		if(!read_spool_u32(spool, &ttl) ||
		   !read_spool_u16(spool, &rdlen)) {
			log_msg(LOG_ERR, "error reading file %s: %s",
				ixfrcr->file_name, strerror(errno));
			return 0;
		}
		/* because rdlen is uint16_t always smaller than sizeof(buf)*/
		if(!fread(buf, rdlen, 1, spool)) {
			log_msg(LOG_ERR, "error reading file %s: %s",
				ixfrcr->file_name, strerror(errno));
			return 0;
		}
		/* see if the rr is in the RRset */
		if(rrset_find_rdata(rrset, ttl, buf, rdlen, &index)) {
			/* it is in both, mark it */
			marked[marked_num++] = index;
		} else {
			/* not in new rrset, but only on spool, it is
			 * a deleted RR */
			if(!ixfr_store_delrr_uncompressed(store,
				(void*)dname_name(domain_dname(domain)),
				domain_dname(domain)->name_size,
				tp, kl, ttl, buf, rdlen)) {
				log_msg(LOG_ERR, "out of memory");
				return 0;
			}
		}
	}
	/* now that we are done, see if RRs in the rrset are not marked,
	 * and thus are new rrs that are added */
	for(i=0; i<rrset->rr_count; i++) {
		int found = 0;
		size_t j;
		for(j=0; j<marked_num; j++) {
			if(marked[j] == i) {
				found = 1;
				break;
			}
		}
		if(found)
			continue;
		/* not in the marked list, the RR is added */
		if(!ixfr_store_addrr_rdatas(store, domain_dname(domain),
			rrset->rrs[i].type, rrset->rrs[i].klass,
			rrset->rrs[i].ttl, rrset->rrs[i].rdatas,
			rrset->rrs[i].rdata_count)) {
			log_msg(LOG_ERR, "out of memory");
			return 0;
		}
	}
	return 1;
}

/* spool read an rrset, it is a deleted RRset */
static int process_spool_delrrset(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, uint8_t* dname, size_t dname_len,
	uint16_t tp, uint16_t kl, uint16_t rrcount)
{
	/* read the RRs from file and add to del list. */
	uint8_t buf[MAX_RDLENGTH];
	int i;
	for(i=0; i<rrcount; i++) {
		uint16_t rdlen;
		uint32_t ttl;
		if(!read_spool_u32(spool, &ttl) ||
		   !read_spool_u16(spool, &rdlen)) {
			log_msg(LOG_ERR, "error reading file %s: %s",
				ixfrcr->file_name, strerror(errno));
			return 0;
		}
		/* because rdlen is uint16_t always smaller than sizeof(buf)*/
		if(!fread(buf, rdlen, 1, spool)) {
			log_msg(LOG_ERR, "error reading file %s: %s",
				ixfrcr->file_name, strerror(errno));
			return 0;
		}
		if(!ixfr_store_delrr_uncompressed(store, dname, dname_len, tp,
			kl, ttl, buf, rdlen)) {
			log_msg(LOG_ERR, "out of memory");
			return 0;
		}
	}
	return 1;
}

/* add the rrset to the added list */
static int process_add_rrset(struct ixfr_store* ixfr_store,
	struct domain* domain, struct rrset* rrset)
{
	int i;
	for(i=0; i<rrset->rr_count; i++) {
		if(!ixfr_store_addrr_rdatas(ixfr_store, domain_dname(domain),
			rrset->rrs[i].type, rrset->rrs[i].klass,
			rrset->rrs[i].ttl, rrset->rrs[i].rdatas,
			rrset->rrs[i].rdata_count)) {
			log_msg(LOG_ERR, "out of memory");
			return 0;
		}
	}
	return 1;
}

/* add the RR types that are not in the marktypes list from the new zone */
static int process_marktypes(struct ixfr_store* store, struct zone* zone,
	struct domain* domain, uint16_t* marktypes, size_t marktypes_used)
{
	/* walk through the rrsets in the zone, if it is not in the
	 * marktypes list, then it is new and an added RRset */
	rrset_type* s;
	size_t i;
	for(s=domain->rrsets; s; s=s->next) {
		uint16_t tp;
		int found = 0;
		if(s->zone != zone)
			continue;
		tp = rrset_rrtype(s);
		for(i=0; i<marktypes_used; i++) {
			if(marktypes[i] == tp) {
				found = 1;
				break;
			}
		}
		if(found)
			continue;
		if(!process_add_rrset(store, domain, s))
			return 0;
	}
	return 1;
}

/* check the difference between the domain and RRs from spool */
static int process_diff_domain(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, struct zone* zone, struct domain* domain)
{
	/* Read the RR types from spool. Mark off the ones seen,
	 * later, the notseen ones from the new zone are added RRsets.
	 * For the ones not in the new zone, they are deleted RRsets.
	 * If they exist in old and new, check for RR differences. */
	uint32_t spool_type_count, i; 
	uint16_t marktypes[65536];
	size_t marktypes_used = 0;
	if(!read_spool_u32(spool, &spool_type_count)) {
		log_msg(LOG_ERR, "error reading file %s: %s",
			ixfrcr->file_name, strerror(errno));
		return 0;
	}
	for(i=0; i<spool_type_count; i++) {
		uint16_t tp, kl, rrcount;
		struct rrset* rrset;
		if(!read_spool_u16(spool, &tp) ||
		   !read_spool_u16(spool, &kl) ||
		   !read_spool_u16(spool, &rrcount)) {
			log_msg(LOG_ERR, "error reading file %s: %s",
				ixfrcr->file_name, strerror(errno));
			return 0;
		}
		rrset = domain_find_rrset(domain, zone, tp);
		if(!rrset) {
			/* rrset in spool but not in new zone, deleted RRset */
			if(!process_spool_delrrset(spool, ixfrcr, store,
				(void*)dname_name(domain_dname(domain)),
				domain_dname(domain)->name_size, tp, kl,
				rrcount))
				return 0;
		} else {
			/* add to the marked types, this one is present in
			 * spool */
			marktypes[marktypes_used++] = tp;
			/* rrset in old and in new zone, diff the RRset */
			if(!process_diff_rrset(spool, ixfrcr, store, domain,
				tp, kl, rrcount, rrset))
				return 0;
		}
	}
	/* process markoff to see if new zone has RRsets not in spool,
	 * those are added RRsets. */
	if(!process_marktypes(store, zone, domain, marktypes, marktypes_used))
		return 0;
	return 1;
}

/* add the RRs for the domain in new zone */
static int process_domain_add_RRs(struct ixfr_store* store, struct zone* zone,
	struct domain* domain)
{
	rrset_type* s;
	for(s=domain->rrsets; s; s=s->next) {
		if(s->zone != zone)
			continue;
		if(!process_add_rrset(store, domain, s))
			return 0;
	}
	return 1;
}

/* del the RRs for the domain from the spool */
static int process_domain_del_RRs(struct ixfr_create* ixfrcr,
	struct ixfr_store* store, FILE* spool, uint8_t* dname,
	size_t dname_len)
{
	uint32_t spool_type_count, i;
	if(!read_spool_u32(spool, &spool_type_count)) {
		log_msg(LOG_ERR, "error reading file %s: %s",
			ixfrcr->file_name, strerror(errno));
		return 0;
	}
	for(i=0; i<spool_type_count; i++) {
		uint16_t tp, kl, rrcount;
		if(!read_spool_u16(spool, &tp) ||
		   !read_spool_u16(spool, &kl) ||
		   !read_spool_u16(spool, &rrcount)) {
			log_msg(LOG_ERR, "error reading file %s: %s",
				ixfrcr->file_name, strerror(errno));
			return 0;
		}
		if(!process_spool_delrrset(spool, ixfrcr, store, dname,
			dname_len, tp, kl, rrcount))
			return 0;
	}
	return 1;
}

/*
 * Structure to keep track of spool domain name iterator.
 * This reads from the spool file and steps over the domain name
 * elements one by one. It keeps track of: is the first one read yet,
 * are we at end nothing more, is the element processed yet that is
 * current read into the buffer?
 */
struct spool_dname_iterator {
	/* the domain name that has recently been read, but can be none
	 * if before first or after last. */
	uint8_t dname[MAXDOMAINLEN+1];
	/* length of the dname, if one is read, otherwise 0 */
	size_t dname_len;
	/* if we are before the first element, hence nothing is read yet */
	int read_first;
	/* if we are after the last element, nothing to read, end of file */
	int eof;
	/* is the element processed that is currently in dname? */
	int is_processed;
	/* the file to read from */
	FILE* spool;
	/* filename for error printout */
	char* file_name;
};

/* init the spool dname iterator */
static void spool_dname_iter_init(struct spool_dname_iterator* iter,
	FILE* spool, char* file_name)
{
	memset(iter, 0, sizeof(*iter));
	iter->spool = spool;
	iter->file_name = file_name;
}

/* read the dname element into the buffer for the spool dname iterator */
static int spool_dname_iter_read(struct spool_dname_iterator* iter)
{
	if(!read_spool_dname(iter->spool, iter->dname, sizeof(iter->dname),
		&iter->dname_len)) {
		log_msg(LOG_ERR, "error reading file %s: %s",
			iter->file_name, strerror(errno));
		return 0;
	}
	return 1;
}

/* get the next name to operate on, that is not processed yet, 0 on failure
 * returns okay on endoffile, check with eof for that.
 * when done with an element, set iter->is_processed on the element. */
static int spool_dname_iter_next(struct spool_dname_iterator* iter)
{
	if(iter->eof)
		return 1;
	if(!iter->read_first) {
		/* read the first one */
		if(!spool_dname_iter_read(iter))
			return 0;
		if(iter->dname_len == 0)
			iter->eof = 1;
		iter->read_first = 1;
		iter->is_processed = 0;
	}
	if(!iter->is_processed) {
		/* the current one needs processing */
		return 1;
	}
	/* read the next one */
	if(!spool_dname_iter_read(iter))
		return 0;
	if(iter->dname_len == 0)
		iter->eof = 1;
	iter->is_processed = 0;
	return 1;
}

/* process the spool input before the domain */
static int process_spool_before_domain(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, struct domain* domain,
	struct spool_dname_iterator* iter, struct region* tmp_region)
{
	const dname_type* dname;
	/* read the domains and rrsets before the domain and those are from
	 * the old zone. If the domain is equal, return to have that processed
	 * if we bypass, that means the domain does not exist, do that */
	while(!iter->eof) {
		if(!spool_dname_iter_next(iter))
			return 0;
		if(iter->eof)
			break;
		/* see if we are at, before or after the domain */
		dname = dname_make(tmp_region, iter->dname, 1);
		if(!dname) {
			log_msg(LOG_ERR, "error in dname in %s",
				iter->file_name);
			return 0;
		}
		if(dname_compare(dname, domain_dname(domain)) < 0) {
			/* the dname is smaller than the one from the zone.
			 * it must be deleted, process it */
			if(!process_domain_del_RRs(ixfrcr, store, spool,
				iter->dname, iter->dname_len))
				return 0;
			iter->is_processed = 1;
		} else {
			/* we are at or after the domain we are looking for,
			 * done here */
			return 1;
		}
	}
	/* no more domains on spool, done here */
	return 1;
}

/* process the spool input for the domain */
static int process_spool_for_domain(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, struct zone* zone, struct domain* domain,
	struct spool_dname_iterator* iter, struct region* tmp_region)
{
	/* process all the spool that is not the domain, that is before the
	 * domain in the new zone */
	if(!process_spool_before_domain(spool, ixfrcr, store, domain, iter,
		tmp_region))
		return 0;
	
	/* are we at the correct domain now? */
	if(iter->eof)
		return 1;
	if(iter->dname_len != domain_dname(domain)->name_size ||
		memcmp(iter->dname, dname_name(domain_dname(domain)),
			iter->dname_len) != 0) {
		/* the domain from the new zone is not present in the old zone,
		 * the content is in the added RRs set */
		if(!process_domain_add_RRs(store, zone, domain))
			return 0;
		return 1;
	}

	/* process the domain */
	/* the domain exists both in the old and new zone,
	 * check for RR differences */
	if(!process_diff_domain(spool, ixfrcr, store, zone, domain))
		return 0;
	iter->is_processed = 1;

	return 1;
}

/* process remaining spool items */
static int process_spool_remaining(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, struct spool_dname_iterator* iter)
{
	/* the remaining domain names in the spool file, that is after
	 * the last domain in the new zone. */
	while(!iter->eof) {
		if(!spool_dname_iter_next(iter))
			return 0;
		if(iter->eof)
			break;
		/* the domain only exists in the spool, the old zone,
		 * and not in the new zone. That would be domains
		 * after the new zone domains, or there are no new
		 * zone domains */
		if(!process_domain_del_RRs(ixfrcr, store, spool, iter->dname,
			iter->dname_len))
			return 0;
		iter->is_processed = 1;
	}
	return 1;
}

/* walk through the zone and find the differences */
static int ixfr_create_walk_zone(FILE* spool, struct ixfr_create* ixfrcr,
	struct ixfr_store* store, struct zone* zone)
{
	struct domain* domain;
	struct spool_dname_iterator iter;
	struct region* tmp_region;
	spool_dname_iter_init(&iter, spool, ixfrcr->file_name);
	tmp_region = region_create(xalloc, free);
	for(domain = zone->apex; domain && domain_is_subdomain(domain,
		zone->apex); domain = domain_next(domain)) {
		uint32_t count = domain_count_rrsets(domain, zone);
		if(count == 0)
			continue;

		/* the domain is a domain in the new zone */
		if(!process_spool_for_domain(spool, ixfrcr, store, zone,
			domain, &iter, tmp_region)) {
			region_destroy(tmp_region);
			return 0;
		}
		region_free_all(tmp_region);
	}
	if(!process_spool_remaining(spool, ixfrcr, store, &iter)) {
		region_destroy(tmp_region);
		return 0;
	}
	region_destroy(tmp_region);
	return 1;
}

int ixfr_create_perform(struct ixfr_create* ixfrcr, struct zone* zone)
{
	struct ixfr_store store_mem, *store;
	FILE* spool;
	spool = fopen(ixfrcr->file_name, "r");
	if(!spool) {
		log_msg(LOG_ERR, "could not open %s for reading: %s",
			ixfrcr->file_name, strerror(errno));
		return 0;
	}
	if(!read_spool_header(spool, ixfrcr)) {
		fclose(spool);
		return 0;
	}
	ixfrcr->new_serial = zone_get_current_serial(zone);
	store = ixfr_store_start(zone, &store_mem, ixfrcr->old_serial,
		ixfrcr->new_serial);

	if(!ixfr_create_walk_zone(spool, ixfrcr, store, zone)) {
		fclose(spool);
		ixfr_store_free(store);
		return 0;
	}

	ixfr_store_free(store);
	fclose(spool);
	return 1;
}