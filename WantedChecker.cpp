#include "WantedChecker.h"
#include "Log.h"
#include <dlfcn.h>
#include <errno.h>
#include <string.h>



static const char shlib_name[] = "wanted_check_api.so";



////////////////////////////////////////////////////////////////////////////////
// A set of no-op builtin "callouts"

static WantedCheckApi::DomainCheckResult noop_check_domain(const std::string &/*domain*/) {
	WantedCheckApi::DomainCheckResult result;
	result.wanted = true;
	return result;
}

static WantedCheckApi::UrlCheckResult noop_check_url(const std::string &/*url*/) {
	WantedCheckApi::UrlCheckResult result;
	result.wanted = true;
	return result;
}

static WantedCheckApi::SingleContentCheckResult noop_check_single_content(const std::string &/*url*/, const void */*content*/, size_t /*content_len*/) {
	WantedCheckApi::SingleContentCheckResult result;
	result.wanted = true;
	return result;
}

static WantedCheckApi::MultiContentCheckResult noop_check_multi_content(const std::vector<WantedCheckApi::MultiContent> &/*content*/) {
	WantedCheckApi::MultiContentCheckResult result;
	result.result = result.wanted;
	return result;
}




//Handle the the loaded shlib
static void *p_shlib = 0;

//The effective descriptor (always contains non-null function pointers)
static WantedCheckApi::APIDescriptorBlock effective_descriptor_block = {
	noop_check_domain,
	noop_check_url,
	noop_check_single_content,
	noop_check_multi_content,
};



bool WantedChecker::initialize() {
	log(LOG_INFO,"Initializing wanted-checking");
	p_shlib = dlopen(shlib_name, RTLD_NOW|RTLD_LOCAL);
	
	if(p_shlib==0) {
		log(LOG_WARN,"Initializing wanted-checking: '%s' could not be loaded (%s)", shlib_name, dlerror());
		return true;
	}
	
	const void *p_descriptor = dlsym(p_shlib,"wanted_check_api_descriptor_block");
	if(!p_descriptor) {
		log(LOG_WARN,"wanted-checking: shlib does not contain the symbol 'wanted_check_api_descriptor_block'");
		dlclose(p_shlib);
		p_shlib = 0;
		return true;
	}
	
	const WantedCheckApi::APIDescriptorBlock *desc = reinterpret_cast<const WantedCheckApi::APIDescriptorBlock*>(p_descriptor);
	
	if(desc->check_domain_pfn)
		effective_descriptor_block.check_domain_pfn = desc->check_domain_pfn;
	if(desc->check_url_pfn)
		effective_descriptor_block.check_url_pfn = desc->check_url_pfn;
	if(desc->check_single_content_pfn)
		effective_descriptor_block.check_single_content_pfn = desc->check_single_content_pfn;
	if(desc->check_multi_content_pfn)
		effective_descriptor_block.check_multi_content_pfn = desc->check_multi_content_pfn;
	
	log(LOG_INFO,"Initialized wanted-checking");
	return true;
}


void WantedChecker::finalize() {
	log(LOG_INFO,"Finalizing wanted-checking");
	
	effective_descriptor_block.check_domain_pfn = noop_check_domain;
	effective_descriptor_block.check_url_pfn = noop_check_url;
	effective_descriptor_block.check_single_content_pfn = noop_check_single_content;
	effective_descriptor_block.check_multi_content_pfn = noop_check_multi_content;
	
	if(p_shlib) {
		dlclose(p_shlib);
		p_shlib = 0;
	}
	
	log(LOG_INFO,"Finalized wanted-checking");
}


WantedChecker::DomainCheckResult WantedChecker::check_domain(const std::string &domain) {
	return effective_descriptor_block.check_domain_pfn(domain);
}


WantedChecker::UrlCheckResult WantedChecker::check_url(const std::string &url) {
	return effective_descriptor_block.check_url_pfn(url);
}

WantedChecker::SingleContentCheckResult WantedChecker::check_single_content(const std::string &url, const void *content, size_t content_len) {
	return effective_descriptor_block.check_single_content_pfn(url,content,content_len);
}
