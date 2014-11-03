#ifndef WIN32
#include <libxml/parser.h>
#include <libxml/xmlschemas.h>
#include <libxml/xmlmemory.h>
#include <libxml/debugXML.h>
#include <libxml/HTMLtree.h>
#include <libxml/xmlIO.h>
#include <libxml/xinclude.h>
#include <libxml/catalog.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>
#endif //WIN32

#ifdef WIN32
    #include <windows.h>
    #include <Shlwapi.h>
    #pragma comment(lib, "shlwapi.lib")
#endif // WIN32

#include "siemensraw.h"
#include "base64.h"
#include "XNode.h"
#include "ConverterXml.h"

#include "ismrmrd/ismrmrd.h"
#include "ismrmrd/dataset.h"

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <iomanip>

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <utility>

#include <typeinfo>

// defined in generated defaults.cpp
extern void initializeEmbeddedFiles(void);
extern std::map<std::string, std::string> global_embedded_files;


struct MysteryData
{
    char mysteryData[160];
};

//////////////////////////////////////////
// These are used in converters         //
// hdf5 style variable length sequences //
//////////////////////////////////////////
typedef struct {
    size_t len; /* Length of VL data (in base type units) */
    void *p;    /* Pointer to VL data */
} hvl_t;

struct sChannelHeader_with_data
{
	sChannelHeader channelHeader;
	hvl_t data;
};

void ClearsChannelHeader_with_data(sChannelHeader_with_data* b)
{
	if (b->data.len) {
		if (b->data.p) {
			complex_float_t* ptr = reinterpret_cast<complex_float_t*>(b->data.p);
			delete [] ptr;
		}
		b->data.p = 0;
		b->data.len = 0;
	}
}

struct sScanHeader_with_data
{
	sScanHeader scanHeader;
	hvl_t data;
};

struct sScanHeader_with_syncdata
{
	sScanHeader scanHeader;
	uint32_t last_scan_counter;
	hvl_t syncdata;
};

void ClearsScanHeader_with_data(sScanHeader_with_data* c)
{
	if (c->data.len) {
		if (c->data.p) {
			for (unsigned int i = 0; i < c->data.len; i++) {
				sChannelHeader_with_data* ptr = reinterpret_cast<sChannelHeader_with_data*>(c->data.p);
				ClearsChannelHeader_with_data(ptr+i);
			}
		}
		c->data.p = 0;
		c->data.len = 0;
	}
}

struct MeasurementHeaderBuffer
{
	hvl_t bufName_;
	uint32_t bufLen_;
	hvl_t buf_;
};

void ClearMeasurementHeaderBuffer(MeasurementHeaderBuffer* b)
{
	if (b->bufName_.len) {
		if (b->bufName_.p) {
			char* ptr = reinterpret_cast<char*>(b->bufName_.p);
			delete [] ptr;
		}
		b->bufName_.p = 0;
		b->bufName_.len = 0;
	}

	if (b->buf_.len) {
		if (b->buf_.p) {
			char* ptr = reinterpret_cast<char*>(b->buf_.p);
			delete [] ptr;
		}
		b->buf_.len = 0;
		b->buf_.p = 0;
	}
}

struct MeasurementHeader
{

public:
	uint32_t dma_length;
	uint32_t nr_buffers;
	hvl_t buffers;

};

void ClearMeasurementHeader(MeasurementHeader* h)
{
	if (h->buffers.len) {
		if (h->buffers.p) {
			MeasurementHeaderBuffer* ptr = reinterpret_cast<MeasurementHeaderBuffer*>(h->buffers.p);
			for (unsigned int i = 0; i < h->buffers.len; i++) {
				ClearMeasurementHeaderBuffer(ptr+i);
			}
		}
		h->buffers.p = 0;
		h->buffers.len = 0;
	}
}

void calc_vds(double slewmax,double gradmax,double Tgsample,double Tdsample,int Ninterleaves,
    double* fov, int numfov,double krmax,
    int ngmax, double** xgrad,double** ygrad,int* numgrad);


void calc_traj(double* xgrad, double* ygrad, int ngrad, int Nints, double Tgsamp, double krmax,
    double** x_trajectory, double** y_trajectory,
    double** weights);


#ifndef WIN32
int xml_file_is_valid(std::string& xml, std::string& schema_file)
{
    xmlDocPtr doc;
    //parse an XML in-memory block and build a tree.
    doc = xmlParseMemory(xml.c_str(), xml.size());

    xmlDocPtr schema_doc;
    //parse an XML in-memory block and build a tree.
    schema_doc = xmlParseMemory(schema_file.c_str(), schema_file.size());

    //Create an XML Schemas parse context for that document. NB. The document may be modified during the parsing process.
    xmlSchemaParserCtxtPtr parser_ctxt = xmlSchemaNewDocParserCtxt(schema_doc);
    if (parser_ctxt == NULL)
    {
        /* unable to create a parser context for the schema */
        xmlFreeDoc(schema_doc);
        return -2;
    }

    //parse a schema definition resource and build an internal XML Shema struture which can be used to validate instances.
    xmlSchemaPtr schema = xmlSchemaParse(parser_ctxt);
    if (schema == NULL)
    {
        /* the schema itself is not valid */
        xmlSchemaFreeParserCtxt(parser_ctxt);
        xmlFreeDoc(schema_doc);
        return -3;
    }

    //Create an XML Schemas validation context based on the given schema.
    xmlSchemaValidCtxtPtr valid_ctxt = xmlSchemaNewValidCtxt(schema);
    if (valid_ctxt == NULL)
    {
        /* unable to create a validation context for the schema */
        xmlSchemaFree(schema);
        xmlSchemaFreeParserCtxt(parser_ctxt);
        xmlFreeDoc(schema_doc);
        xmlFreeDoc(doc);
        return -4;
    }

    //Validate a document tree in memory. Takes a schema validation context and a parsed document tree
    int is_valid = (xmlSchemaValidateDoc(valid_ctxt, doc) == 0);
    xmlSchemaFreeValidCtxt(valid_ctxt);
    xmlSchemaFree(schema);
    xmlSchemaFreeParserCtxt(parser_ctxt);
    xmlFreeDoc(schema_doc);
    xmlFreeDoc(doc);

    /* force the return value to be non-negative on success */
    return is_valid ? 1 : 0;
}
#endif //WIN32


std::string get_date_time_string()
{
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );

    std::stringstream str;
    str << timeinfo->tm_year+1900 << "-"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_mon+1
        << "-"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_mday
        << " "
        << std::setw(2) << std::setfill('0') << timeinfo->tm_hour
        << ":"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_min
        << ":"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_sec;

    std::string ret = str.str();

    return ret;
}


bool is_number(const std::string& s)
{
    bool ret = true;
    for (unsigned int i = 0; i < s.size(); i++)
    {
        if (!std::isdigit(s.c_str()[i]))
        {
            ret = false;
            break;
        }
    }
    return ret;
}


std::string ProcessParameterMap(const XProtocol::XNode& node, const char* mapfile)
{
    TiXmlDocument out_doc;

    TiXmlDeclaration* decl = new TiXmlDeclaration( "1.0", "", "" );
    out_doc.LinkEndChild( decl );

    ConverterXMLNode out_n(&out_doc);

    //Input document
    TiXmlDocument doc;
    doc.Parse(mapfile);
    TiXmlHandle docHandle(&doc);

    TiXmlElement* parameters = docHandle.FirstChildElement("siemens").FirstChildElement("parameters").ToElement();
    if (parameters)
    {
        TiXmlNode* p = 0;
        while((p = parameters->IterateChildren( "p",  p )))
        {
            TiXmlHandle ph(p);

            TiXmlText* s = ph.FirstChildElement("s").FirstChild().ToText();
            TiXmlText* d = ph.FirstChildElement("d").FirstChild().ToText();

            if (s && d)
            {
                std::string source      = s->Value();
                std::string destination = d->Value();

                std::vector<std::string> split_path;
                boost::split( split_path, source, boost::is_any_of("."), boost::token_compress_on );

                if (is_number(split_path[0]))
                {
                    std::cout << "First element of path (" << source << ") cannot be numeric" << std::endl;
                    continue;
                }

                std::string search_path = split_path[0];
                for (unsigned int i = 1; i < split_path.size()-1; i++)
                {
                    /*
                    if (is_number(split_path[i]) && (i != split_path.size())) {
                    std::cout << "Numeric index not supported inside path for source = " << source << std::endl;
                    continue;
                    }*/

                    search_path += std::string(".") + split_path[i];
                }

                int index = -1;
                if (is_number(split_path[split_path.size()-1]))
                {
                    index = atoi(split_path[split_path.size()-1].c_str());
                }
                else
                {
                    search_path += std::string(".") + split_path[split_path.size()-1];
                }

                const XProtocol::XNode* n = boost::apply_visitor(XProtocol::getChildNodeByName(search_path), node);

                std::vector<std::string> parameters;
                if (n)
                {
                    parameters = boost::apply_visitor(XProtocol::getStringValueArray(), *n);
                }
                else
                {
                    std::cout << "Search path: " << search_path << " not found." << std::endl;
                }

                if (index >= 0)
                {
                    if (parameters.size() > index)
                    {
                        out_n.add(destination, parameters[index]);
                    }
                    else
                    {
                        std::cout << "Parameter index (" << index << ") not valid for search path " << search_path << std::endl;
                        continue;
                    }
                }
                else
                {
                    out_n.add(destination, parameters);
                }
            }
            else
            {
                std::cout << "Malformed parameter map" << std::endl;
            }
        }
    }
    else
    {
        std::cout << "Malformed parameter map (parameters section not found)" << std::endl;
        return std::string("");
    }
    return XmlToString(out_doc);
}


/// compute noise dwell time in us for dependency and built-in noise in VD/VB lines
double compute_noise_sample_in_us(size_t num_of_noise_samples_this_acq, bool isAdjustCoilSens, bool isVB)
{
    if ( isAdjustCoilSens )
    {
        return 5.0;
    }
    else if ( isVB )
    {
        return (10e6/num_of_noise_samples_this_acq/130.0);
    }
    else
    {
        return ( ((long)(76800.0/num_of_noise_samples_this_acq)) / 10.0 );
    }

    return 5.0;
}

std::string load_embedded(std::string name)
{
    std::string contents;
    std::map<std::string, std::string>::iterator it = global_embedded_files.find(name);
    if (it != global_embedded_files.end())
    {
        std::string encoded = it->second;
        contents = base64_decode(encoded);
    }
    else
    {
        std::cerr << "ERROR: File " << name << " is not embedded!" << std::endl;
        exit(1);
    }
    return contents;
}

int main(int argc, char *argv[] )
{
    std::string filename;
    unsigned int measurement_number;

    std::string parammap_file;
    std::string parammap_xsl;

    std::string usermap_file;
    std::string usermap_xsl;

    std::string schema_file_name;

    std::string ismrmrd_file;
    std::string ismrmrd_group;
    std::string date_time = get_date_time_string();

    bool debug_xml = false;
    bool flash_pat_ref_scan = false;

    bool list = false;
    std::string to_extract;

    std::string xslt_home;

    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h",                  "Produce HELP message")
        ("file,f",                  po::value<std::string>(&filename), "<SIEMENS dat file>")
        ("measNum,z",               po::value<unsigned int>(&measurement_number)->default_value(1), "<Measurement number>")
        ("pMap,m",                  po::value<std::string>(&parammap_file), "<Parameter map XML file>")
        ("pMapStyle,x",             po::value<std::string>(&parammap_xsl), "<Parameter stylesheet XSL file>")
        ("user-map",                po::value<std::string>(&usermap_file), "<Provide a parameter map XML file>")
        ("user-stylesheet",         po::value<std::string>(&usermap_xsl), "<Provide a parameter stylesheet XSL file>")
        ("output,o",                po::value<std::string>(&ismrmrd_file)->default_value("output.h5"), "<ISMRMRD output file>")
        ("outputGroup,g",           po::value<std::string>(&ismrmrd_group)->default_value("dataset"), "<ISMRMRD output group>")
        ("list,l",                  po::value<bool>(&list)->implicit_value(true), "<List embedded files>")
        ("extract,e",               po::value<std::string>(&to_extract), "<Extract embedded file>")
        ("debug,X",                 po::value<bool>(&debug_xml)->implicit_value(true), "<Debug XML flag>")
        ("flashPatRef,F",           po::value<bool>(&flash_pat_ref_scan)->implicit_value(true), "<FLASH PAT REF flag>")
        ;

    po::options_description display_options("Allowed options");
    display_options.add_options()
        ("help,h",                  "Produce HELP message")
        ("file,f",                  "<SIEMENS dat file>")
        ("measNum,z",               "<Measurement number>")
        ("pMap,m",                  "<Parameter map XML>")
        ("pMapStyle,x",             "<Parameter stylesheet XSL>")
        ("user-map",                "<Provide a parameter map XML file>")
        ("user-stylesheet",         "<Provide a parameter stylesheet XSL file>")
        ("output,o",                "<ISMRMRD output file>")
        ("outputGroup,g",           "<ISMRMRD output group>")
        ("list,l",                  "<List embedded files>")
        ("extract,e",               "<Extract embedded file>")
        ("debug,X",                 "<Debug XML flag>")
        ("flashPatRef,F",           "<FLASH PAT REF flag>")
        ;

    po::variables_map vm;

    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help"))
        {
            std::cout << display_options << "\n";
            return 1;
        }
    }

    catch(po::error& e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
        std::cerr << display_options << std::endl;
        return -1;
    }

    // Add all embedded files to the global_embedded_files map
    initializeEmbeddedFiles();

    if (list)
    {
        std::map<std::string, std::string>::iterator iter;
        std::cout << "Embedded Files: " << std::endl;
        for (iter = global_embedded_files.begin(); iter != global_embedded_files.end(); ++iter)
        {
            if (iter->first != "ismrmrd.xsd")
            {
                std::cout << "    " << iter->first << std::endl;
            }
        }
        return 0;
    }
    else
        if (to_extract.length() > 0)
        {
            std::string contents = load_embedded(to_extract);
            std::ofstream outfile(to_extract.c_str());
            outfile.write(contents.c_str(), contents.size());
            outfile.close();
            std::cout << to_extract << " successfully extracted. " << std::endl;
            return 0;
        }

    if (measurement_number < 1)
    {
        std::cout << "The measurement number must be positive number higher than 0" << std::endl;
        std::cout << display_options << "\n";
        return -1;
    }

    //If Siemens file is not provided, terminate the execution
    if (filename.length() == 0)
    {
        std::cout << display_options << "\n";
        return -1;
    }
    else
    {
        std::ifstream file_1(filename.c_str());
        if (!file_1)
        {
            std::cout << "Provided Siemens file can not be open or does not exist." << std::endl;
            std::cout << display_options << "\n";
            return -1;
        }
        else
        {
            std::cout << "Siemens file is: " << filename << std::endl;
        }
        file_1.close();
    }

    std::string parammap_xsl_content;
    if (parammap_xsl.length() == 0)
    {
        // If the user did not specify any stylesheet
        if (usermap_xsl.length() == 0)
        {
            parammap_xsl_content = load_embedded("IsmrmrdParameterMap_Siemens.xsl");
            std::cout << "Parameter XSL stylesheet is: IsmrmrdParameterMap_Siemens.xsl" << std::endl;
        }
        // If the user specified only a user-supplied stylesheet
        else
        {
            std::ifstream f(usermap_xsl.c_str());
            if (!f)
            {
                std::cerr << "Parameter XSL stylesheet: " << usermap_xsl << " does not exist." << std::endl;
                std::cerr << display_options << "\n";
                return -1;
            }
            else
            {
                std::cout << "Parameter XSL stylesheet is: " << usermap_xsl << std::endl;

                std::string str_f((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                parammap_xsl_content = str_f;
            }
            f.close();
        }
    }
    else
    {
        // If the user specified both an embedded and user-supplied stylesheet
        if (usermap_xsl.length() > 0)
        {
            std::cerr << "Cannot specify a user-supplied parameter map XSL stylesheet AND embedded stylesheet" << std::endl;
            return -1;
        }
        // If the user specified an embedded stylesheet only
        else
        {
            parammap_xsl_content = load_embedded(parammap_xsl);
            std::cout << "Parameter XSL stylesheet is: " << parammap_xsl << std::endl;
        }
    }

    std::string schema_file_name_content = load_embedded("ismrmrd.xsd");

    // Create an ISMRMRD dataset
    ISMRMRD::Dataset ismrmrd_dataset(ismrmrd_file.c_str(), ismrmrd_group.c_str(), true);

    std::ifstream f(filename.c_str(), std::ios::binary);

    MrParcRaidFileHeader ParcRaidHead;

    f.read((char*)(&ParcRaidHead), sizeof(MrParcRaidFileHeader));

    bool VBFILE = false;

    if (ParcRaidHead.hdSize_ > 32)
    {
        VBFILE = true;

        //Rewind, we have no raid file header.
        f.seekg(0, std::ios::beg);

        ParcRaidHead.hdSize_ = ParcRaidHead.count_;
        ParcRaidHead.count_ = 1;
    }

    else if (ParcRaidHead.hdSize_ != 0)
    {
        //This is a VB line data file
        std::cout << "Only VD line files with MrParcRaidFileHeader.hdSize_ == 0 (MR_PARC_RAID_ALLDATA) supported." << std::endl;
        std::cout << display_options << "\n";
        f.close();
        return -1;
    }

    if (!VBFILE && measurement_number > ParcRaidHead.count_)
    {
        std::cout << "The file you are trying to convert has only " << ParcRaidHead.count_ << " measurements." << std::endl;
        std::cout << "You are trying to convert measurement number: " << measurement_number << std::endl;
        std::cout << display_options << "\n";
        f.close();
        return -1;
    }

    //if it is a VB scan
    if (VBFILE && measurement_number != 1)
    {
        std::cout << "The file you are trying to convert is a VB file and it has only one measurement." << std::endl;
        std::cout << "You tried to convert measurement number: " << measurement_number << std::endl;
        std::cout << display_options << "\n";
        f.close();
        return -1;
    }

    std::string parammap_file_content;

    if (VBFILE)
    {
        if (parammap_file.length() == 0)
        {
            // If the user did not specify any parameter map file
            if (usermap_file.length() == 0)
            {
                parammap_file_content = load_embedded("IsmrmrdParameterMap_Siemens_VB17.xml");
                std::cout << "Parameter map file is: IsmrmrdParameterMap_Siemens_VB17.xml" << std::endl;
            }
            // If the user specified only a user-supplied parameter map file
            else
            {
                std::ifstream f(usermap_file.c_str());
                if (!f)
                {
                    std::cerr << "Parameter map file: " << usermap_file << " does not exist." << std::endl;
                    std::cerr << display_options << "\n";
                    return -1;
                }
                else
                {
                    std::cout << "Parameter map file is: " << usermap_file << std::endl;

                    std::string str_f((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    parammap_file_content = str_f;
                }
                f.close();
            }
        }
        else
        {
            // If the user specified both an embedded and user-supplied parameter map file
            if (usermap_file.length() > 0)
            {
                std::cerr << "Cannot specify a user-supplied parameter map XML file AND embedded XML file" << std::endl;
                return -1;
            }
            // If the user specified an embedded parameter map file only
            else
            {
                parammap_file_content = load_embedded(parammap_file);
                std::cout << "Parameter map file is: " << parammap_file << std::endl;
            }
        }
    }

    if (!VBFILE)
    {
        if (parammap_file.length() == 0)
        {
            // If the user did not specify any parameter map file
            if (usermap_file.length() == 0)
            {
                parammap_file_content = load_embedded("IsmrmrdParameterMap_Siemens.xml");
                std::cout << "Parameter map file is: IsmrmrdParameterMap_Siemens.xml" << std::endl;
            }
            // If the user specified only a user-supplied parameter map file
            else
            {
                std::ifstream f(usermap_file.c_str());
                if (!f)
                {
                    std::cerr << "Parameter map file: " << usermap_file << " does not exist." << std::endl;
                    std::cerr << display_options << "\n";
                    return -1;
                }
                else
                {
                    std::cout << "Parameter map file is: " << usermap_file << std::endl;

                    std::string str_f((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    parammap_file_content = str_f;
                }
                f.close();
            }
        }
        else
        {
            // If the user specified both an embedded and user-supplied parameter map file
            if (usermap_file.length() > 0)
            {
                std::cerr << "Cannot specify a user-supplied parameter map XML file AND embedded XML file" << std::endl;
                return -1;
            }
            // If the user specified an embedded parameter map file only
            else
            {
                parammap_file_content = load_embedded(parammap_file);
                std::cout << "Parameter map file is: " << parammap_file << std::endl;
            }
        }
    }

    std::cout << "This file contains " << ParcRaidHead.count_ << " measurement(s)." << std::endl;

    std::vector<MrParcRaidFileEntry> ParcFileEntries(64);

    if (VBFILE)
    {
        std::cout << "VB line file detected." << std::endl;
        //In case of VB file, we are just going to fill these with zeros. It doesn't exist.
        for (unsigned int i = 0; i < 64; i++)
        {
            memset(&ParcFileEntries[i], 0, sizeof(MrParcRaidFileEntry));
        }

        ParcFileEntries[0].off_ = 0;
        f.seekg(0,std::ios::end); //Rewind a bit, we have no raid file header.
        ParcFileEntries[0].len_ = f.tellg(); //This is the whole size of the dat file
        f.seekg(0,std::ios::beg); //Rewind a bit, we have no raid file header.

        std::cout << "Protocol name: " << ParcFileEntries[0].protName_ << std::endl; // blank
    }
    else
    {
        std::cout << "VD line file detected." << std::endl;
        for (unsigned int i = 0; i < 64; i++)
        {
            f.read((char*)(&ParcFileEntries[i]), sizeof(MrParcRaidFileEntry));

            if (i < ParcRaidHead.count_)
            {
                std::cout << "Protocol name: " << ParcFileEntries[i].protName_ << std::endl;
            }
        }
    }

    MysteryData mystery_data;
    MeasurementHeader mhead;

    // find the beggining of the desired measurement
    f.seekg(ParcFileEntries[measurement_number-1].off_, std::ios::beg);

    //MeasurementHeader mhead;
    f.read((char*)(&mhead.dma_length), sizeof(uint32_t));
    f.read((char*)(&mhead.nr_buffers),sizeof(uint32_t));

    //std::cout << "Measurement header DMA length: " << mhead.dma_length << std::endl;

    //Now allocate dynamic memory for the buffers
    mhead.buffers.len = mhead.nr_buffers;

    MeasurementHeaderBuffer* buffers = new MeasurementHeaderBuffer[mhead.nr_buffers];
    mhead.buffers.p = (void*)(buffers);

    std::cout << "Number of parameter buffers: " << mhead.nr_buffers << std::endl;

    char bufname_tmp[32];
    for (int b = 0; b < mhead.nr_buffers; b++)
    {
        f.getline(bufname_tmp, 32, '\0');
        std::cout << "Buffer Name: " << bufname_tmp << std::endl;
        buffers[b].bufName_.len = f.gcount() + 1;
        bufname_tmp[f.gcount()] = '\0';
        buffers[b].bufName_.p = (void*)(new char[buffers[b].bufName_.len]);
        memcpy(buffers[b].bufName_.p, bufname_tmp, buffers[b].bufName_.len);

        f.read((char*)(&buffers[b].bufLen_), sizeof(uint32_t));
        buffers[b].buf_.len = buffers[b].bufLen_;
        buffers[b].buf_.p = (void*)(new char[buffers[b].buf_.len]);
        f.read((char*)(buffers[b].buf_.p), buffers[b].buf_.len);
    }

    //We need to be on a 32 byte boundary after reading the buffers
    long long int position_in_meas = (long long int)(f.tellg()) - ParcFileEntries[measurement_number-1].off_;
    if (position_in_meas % 32)
    {
        f.seekg(32 - (position_in_meas % 32), std::ios::cur);
    }

    // Measurement header done!
    //Now we should have the measurement headers, so let's use the Meas header to create the XML parameters

    std::string xml_config;
    std::vector<std::string> wip_long;
    std::vector<std::string> wip_double;
    long trajectory = 0;
    long dwell_time_0 = 0;
    long max_channels = 0;
    long radial_views = 0;
    long center_line = 0;
    long center_partition = 0;
    long lPhaseEncodingLines = 0;
    long iNoOfFourierLines = 0;
    long lPartitions = 0;
    long iNoOfFourierPartitions = 0;
    std::string seqString;
    std::string baseLineString;

    std::string protocol_name = "";

    for (unsigned int b = 0; b < mhead.nr_buffers; b++)
    {
        if (std::string((char*)buffers[b].bufName_.p).compare("Meas") == 0)
        {
            std::string config_buffer((char*)buffers[b].buf_.p, buffers[b].buf_.len-2);//-2 because the last two character are ^@

            XProtocol::XNode n;

            if (debug_xml)
            {
                std::ofstream o("config_buffer.xprot");
                o.write(config_buffer.c_str(), config_buffer.size());
                o.close();
            }

            if (XProtocol::ParseXProtocol(const_cast<std::string&>(config_buffer),n) < 0)
            {
                std::cout << "Failed to parse XProtocol" << std::endl;
                return -1;
            }

            //Get some parameters - wip long
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sWipMemBlock.alFree"), n);
                if (n2)
                {
                    wip_long = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "Search path: MEAS.sWipMemBlock.alFree not found." << std::endl;
                }
                if (wip_long.size() == 0)
                {
                    std::cout << "Failed to find WIP long parameters" << std::endl;
                    return -1;
                }
            }

            //Get some parameters - wip long
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sWipMemBlock.adFree"), n);
                if (n2)
                {
                    wip_double = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "Search path: MEAS.sWipMemBlock.adFree not found." << std::endl;
                }
                if (wip_double.size() == 0)
                {
                    std::cout << "Failed to find WIP double parameters" << std::endl;
                    return -1;
                }
            }

            //Get some parameters - dwell times
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sRXSPEC.alDwellTime"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "Search path: MEAS.sWipMemBlock.alFree not found." << std::endl;
                }
                if (temp.size() == 0)
                {
                    std::cout << "Failed to find dwell times" << std::endl;
                    return -1;
                }
                else
                {
                    dwell_time_0 = std::atoi(temp[0].c_str());
                }
            }

            //Get some parameters - trajectory
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sKSpace.ucTrajectory"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "Search path: MEAS.sKSpace.ucTrajectory not found." << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find appropriate trajectory array" << std::endl;
                    return -1;
                }
                else
                {
                    trajectory = std::atoi(temp[0].c_str());
                    std::cout << "Trajectory is: " << trajectory << std::endl;
                }
            }

            //Get some parameters - max channels
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("YAPS.iMaxNoOfRxChannels"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "YAPS.iMaxNoOfRxChannels" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find YAPS.iMaxNoOfRxChannels array" << std::endl;
                    return -1;
                }
                else
                {
                    max_channels = std::atoi(temp[0].c_str());
                }
            }

            //Get some parameters - cartesian encoding bits
            {
                // get the center line parameters
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sKSpace.lPhaseEncodingLines"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "MEAS.sKSpace.lPhaseEncodingLines not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find MEAS.sKSpace.lPhaseEncodingLines array" << std::endl;
                    return -1;
                }
                else
                {
                    lPhaseEncodingLines = std::atoi(temp[0].c_str());
                }

                n2 = boost::apply_visitor(XProtocol::getChildNodeByName("YAPS.iNoOfFourierLines"), n);
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "YAPS.iNoOfFourierLines not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find YAPS.iNoOfFourierLines array" << std::endl;
                    return -1;
                }
                else
                {
                    iNoOfFourierLines = std::atoi(temp[0].c_str());
                }

                long lFirstFourierLine;
                bool has_FirstFourierLine = false;
                n2 = boost::apply_visitor(XProtocol::getChildNodeByName("YAPS.lFirstFourierLine"), n);
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "YAPS.lFirstFourierLine not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find YAPS.lFirstFourierLine array" << std::endl;
                    has_FirstFourierLine = false;
                }
                else
                {
                    lFirstFourierLine = std::atoi(temp[0].c_str());
                    has_FirstFourierLine = true;
                }

                // get the center partition parameters
                n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sKSpace.lPartitions"), n);
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "MEAS.sKSpace.lPartitions not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find MEAS.sKSpace.lPartitions array" << std::endl;
                    return -1;
                }
                else
                {
                    lPartitions = std::atoi(temp[0].c_str());
                }

                // Note: iNoOfFourierPartitions is sometimes absent for 2D sequences
                n2 = boost::apply_visitor(XProtocol::getChildNodeByName("YAPS.iNoOfFourierPartitions"), n);
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                    if (temp.size() != 1)
                    {
                        iNoOfFourierPartitions = 1;
                    }
                    else
                    {
                        iNoOfFourierPartitions = std::atoi(temp[0].c_str());
                    }
                }
                else
                {
                    iNoOfFourierPartitions = 1;
                }

                long lFirstFourierPartition;
                bool has_FirstFourierPartition = false;
                n2 = boost::apply_visitor(XProtocol::getChildNodeByName("YAPS.lFirstFourierPartition"), n);
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "YAPS.lFirstFourierPartition not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find YAPS.lFirstFourierPartition array" << std::endl;
                    has_FirstFourierPartition = false;
                }
                else
                {
                    lFirstFourierPartition = std::atoi(temp[0].c_str());
                    has_FirstFourierPartition = true;
                }

                // set the values
                if ( has_FirstFourierLine ) // bottom half for partial fourier
                {
                    center_line = lPhaseEncodingLines/2 - ( lPhaseEncodingLines - iNoOfFourierLines );
                }
                else
                {
                    center_line = lPhaseEncodingLines/2;
                }

                if (iNoOfFourierPartitions > 1) {
                    // 3D
                    if ( has_FirstFourierPartition ) // bottom half for partial fourier
                    {
                        center_partition = lPartitions/2 - ( lPartitions - iNoOfFourierPartitions );
                    }
                    else
                    {
                        center_partition = lPartitions/2;
                    }
                } else {
                    // 2D
                    center_partition = 0;
                }

                // for spiral sequences the center_line and center_partition are zero
                if (trajectory == 4) {
                    center_line = 0;
                    center_partition = 0;
                }

                std::cout << "center_line = " << center_line << std::endl;
                std::cout << "center_partition = " << center_partition << std::endl;
            }

            //Get some parameters - radial views
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sKSpace.lRadialViews"), n);
                std::vector<std::string> temp;
                if (n2) {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                } else {
                    std::cout << "MEAS.sKSpace.lRadialViews not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find YAPS.MEAS.sKSpace.lRadialViews array" << std::endl;
                    return -1;
                }
                else
                {
                    radial_views = std::atoi(temp[0].c_str());
                }
            }

            //Get some parameters - protocol name
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("HEADER.tProtocolName"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                else
                {
                    std::cout << "HEADER.tProtocolName not found" << std::endl;
                }
                if (temp.size() != 1)
                {
                    std::cout << "Failed to find HEADER.tProtocolName" << std::endl;
                    return -1;
                }
                else
                {
                    protocol_name = temp[0];
                }
            }

            // Get some parameters - base line
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sProtConsistencyInfo.tBaselineString"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                if (temp.size() > 0)
                {
                    baseLineString = temp[0];
                }
            }

            if ( baseLineString.empty() )
            {
                const XProtocol::XNode* n2 = boost::apply_visitor(XProtocol::getChildNodeByName("MEAS.sProtConsistencyInfo.tMeasuredBaselineString"), n);
                std::vector<std::string> temp;
                if (n2)
                {
                    temp = boost::apply_visitor(XProtocol::getStringValueArray(), *n2);
                }
                if (temp.size() > 0)
                {
                    baseLineString = temp[0];
                }
            }

            if ( baseLineString.empty() )
            {
                std::cout << "Failed to find MEAS.sProtConsistencyInfo.tBaselineString/tMeasuredBaselineString" << std::endl;
            }

            //xml_config = ProcessParameterMap(n, parammap_file);
            xml_config = ProcessParameterMap(n, parammap_file_content.c_str());

            break;
        }
    }

    // whether this scan is a adjustment scan
    bool isAdjustCoilSens = false;
    if ( protocol_name == "AdjCoilSens" )
    {
        isAdjustCoilSens = true;
    }

    // whether this scan is from VB line
    bool isVB = false;
    if ( (baseLineString.find("VB17") != std::string::npos)
        || (baseLineString.find("VB15") != std::string::npos)
        || (baseLineString.find("VB13") != std::string::npos)
        || (baseLineString.find("VB11") != std::string::npos) )
    {
        isVB = true;
    }

    std::cout << "Baseline: " << baseLineString << std::endl;

    if (debug_xml)
    {
        std::ofstream o("xml_raw.xml");
        o.write(xml_config.c_str(), xml_config.size());
        o.close();
    }

    //Get rid of dynamically allocated memory in header
    {
        ClearMeasurementHeader(&mhead);
    }

#ifndef WIN32
    xsltStylesheetPtr cur = NULL;

    xmlDocPtr doc, res, xml_doc;

    const char *params[16 + 1];

    int nbparams = 0;

    params[nbparams] = NULL;

    xmlSubstituteEntitiesDefault(1);

    xmlLoadExtDtdDefaultValue = 1;

    xml_doc = xmlParseMemory(parammap_xsl_content.c_str(), parammap_xsl_content.size());

    if (xml_doc == NULL)
    {
        std::cout << "Error when parsing xsl parameter stylesheet..." << std::endl;
        return -1;
    }

    cur = xsltParseStylesheetDoc(xml_doc);
    doc = xmlParseMemory(xml_config.c_str(), xml_config.size());
    res = xsltApplyStylesheet(cur, doc, params);

    xmlChar* out_ptr = NULL;
    int xslt_length = 0;
    int xslt_result = xsltSaveResultToString(&out_ptr, &xslt_length, res, cur);

    if (xslt_result < 0)
    {
        std::cout << "Failed to save converted doc to string" << std::endl;
        return -1;
    }

    xml_config = std::string((char*)out_ptr,xslt_length);

    if (debug_xml)
    {
        std::ofstream o("processed.xml");
        o.write(xml_config.c_str(), xml_config.size());
        o.close();
    }

    if (xml_file_is_valid(xml_config, schema_file_name_content) <= 0)
    {
        std::cout << "Generated XML is not valid according to the ISMRMRD schema" << std::endl;
        return -1;
    }

    xsltFreeStylesheet(cur);
    xmlFreeDoc(res);
    xmlFreeDoc(doc);

    xsltCleanupGlobals();
    xmlCleanupParser();

#else
    std::string syscmd;
    int xsltproc_res(0);

    std::string xml_post("xml_post.xml"), xml_pre("xml_pre.xml");

    // Full path to the executable (including the executable file)
    char fullPath[MAX_PATH];
	
    // Full path to the executable (without executable file)
    char *rightPath;
    
    // Will contain exe path
    HMODULE hModule = GetModuleHandle(NULL);
    if (hModule != NULL)
    {
	    // When passing NULL to GetModuleHandle, it returns handle of exe itself
	    GetModuleFileName(hModule, fullPath, (sizeof(fullPath))); 

		rightPath = fullPath;
		
		PathRemoveFileSpec(rightPath);
    }
    else
    {
        std::cout << "The path to the executable is NULL" << std::endl;
    }
	
    std::ofstream xslf("xsl_file");
    xslf.write(parammap_xsl_content.c_str(), parammap_xsl_content.size());
    xslf.close();
	
    syscmd = std::string(rightPath) + std::string("\\") + std::string("xsltproc --output xml_post.xml \"") + std::string("xsl_file") + std::string("\" xml_pre.xml");

    std::ofstream o(xml_pre.c_str());
    o.write(xml_config.c_str(), xml_config.size());
    o.close();

    xsltproc_res = system(syscmd.c_str());

    std::ifstream t(xml_post.c_str());
    xml_config = std::string((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());

    if ( xsltproc_res != 0 )
    {
        std::cerr << "Failed to call up xsltproc : \t" << syscmd << std::endl;

        std::ofstream o(xml_pre.c_str());
        o.write(xml_config.c_str(), xml_config.size());
        o.close();

        xsltproc_res = system(syscmd.c_str());

        if ( xsltproc_res != 0 )
        {
            std::cerr << "Failed to generate XML header" << std::endl;
            return -1;
        }

        std::ifstream t(xml_post.c_str());
        xml_config = std::string((std::istreambuf_iterator<char>(t)),
            std::istreambuf_iterator<char>());
    }
#endif //WIN32

    ISMRMRD::AcquisitionHeader acq_head_base;
    ismrmrd_dataset.writeHeader(xml_config);

    //If this is a spiral acquisition, we will calculate the trajectory and add it to the individual profiles
    ISMRMRD::NDArray<float> traj;
     std::vector<size_t> traj_dim;
     if (trajectory == 4)
     {
         int     nfov   = 1;         /*  number of fov coefficients.             */
         int     ngmax  = (int)1e5;  /*  maximum number of gradient samples      */
         double  *xgrad;             /*  x-component of gradient.                */
         double  *ygrad;             /*  y-component of gradient.                */
         double  *x_trajectory;
         double  *y_trajectory;
         double  *weighting;
         int     ngrad;

         double sample_time = (1.0*dwell_time_0) * 1e-9;
         double smax = std::atof(wip_double[7].c_str());
         double gmax = std::atof(wip_double[6].c_str());
         double fov = std::atof(wip_double[9].c_str());
         double krmax = std::atof(wip_double[8].c_str());
         long interleaves = radial_views;

         /*    call c-function here to calculate gradients */
         calc_vds(smax, gmax, sample_time, sample_time, interleaves, &fov, nfov, krmax, ngmax, &xgrad, &ygrad, &ngrad);

         /*
         std::cout << "Calculated trajectory for spiral: " << std::endl
         << "sample_time: " << sample_time << std::endl
         << "smax: " << smax << std::endl
         << "gmax: " << gmax << std::endl
         << "fov: " << fov << std::endl
         << "krmax: " << krmax << std::endl
         << "interleaves: " << interleaves << std::endl
         << "ngrad: " << ngrad << std::endl;
         */

         /* Calcualte the trajectory and weights*/
         calc_traj(xgrad, ygrad, ngrad, interleaves, sample_time, krmax, &x_trajectory, &y_trajectory, &weighting);

         // 2 * number of points for each X and Y
         traj_dim.push_back(2);
         traj_dim.push_back(ngrad);
         traj_dim.push_back(interleaves);
         traj.resize(traj_dim);

         for (int i = 0; i < (ngrad*interleaves); i++)
         {
             traj.getData()[i * 2] = (float)(-x_trajectory[i]/2);
             traj.getData()[i * 2 + 1] = (float)(-y_trajectory[i]/2);
         }

         delete [] xgrad;
         delete [] ygrad;
         delete [] x_trajectory;
         delete [] y_trajectory;
         delete [] weighting;

     }

     uint32_t last_mask = 0;
     unsigned long int acquisitions = 1;
     unsigned long int sync_data_packets = 0;
     sMDH mdh;//For VB line
     bool first_call = true;

     while (!(last_mask & 1) && //Last scan not encountered
             (((ParcFileEntries[measurement_number-1].off_+ ParcFileEntries[measurement_number-1].len_)-f.tellg()) > sizeof(sScanHeader)))  //not reached end of measurement without acqend
     {
         size_t position_in_meas = f.tellg();
         sScanHeader_with_data scanhead;
         f.read(reinterpret_cast<char*>(&scanhead.scanHeader.ulFlagsAndDMALength), sizeof(uint32_t));

         if (VBFILE)
         {
             f.read(reinterpret_cast<char*>(&mdh) + sizeof(uint32_t), sizeof(sMDH) - sizeof(uint32_t));
             scanhead.scanHeader.lMeasUID = mdh.lMeasUID;
             scanhead.scanHeader.ulScanCounter = mdh.ulScanCounter;
             scanhead.scanHeader.ulTimeStamp = mdh.ulTimeStamp;
             scanhead.scanHeader.ulPMUTimeStamp = mdh.ulPMUTimeStamp;
             scanhead.scanHeader.ushSystemType = 0;
             scanhead.scanHeader.ulPTABPosDelay = 0;
             scanhead.scanHeader.lPTABPosX = 0;
             scanhead.scanHeader.lPTABPosY = 0;
             scanhead.scanHeader.lPTABPosZ = mdh.ushPTABPosNeg;//TODO: Modify calculation
             scanhead.scanHeader.ulReserved1 = 0;
             scanhead.scanHeader.aulEvalInfoMask[0] = mdh.aulEvalInfoMask[0];
             scanhead.scanHeader.aulEvalInfoMask[1] = mdh.aulEvalInfoMask[1];
             scanhead.scanHeader.ushSamplesInScan = mdh.ushSamplesInScan;
             scanhead.scanHeader.ushUsedChannels = mdh.ushUsedChannels;
             scanhead.scanHeader.sLC = mdh.sLC;
             scanhead.scanHeader.sCutOff = mdh.sCutOff;
             scanhead.scanHeader.ushKSpaceCentreColumn = mdh.ushKSpaceCentreColumn;
             scanhead.scanHeader.ushCoilSelect = mdh.ushCoilSelect;
             scanhead.scanHeader.fReadOutOffcentre = mdh.fReadOutOffcentre;
             scanhead.scanHeader.ulTimeSinceLastRF = mdh.ulTimeSinceLastRF;
             scanhead.scanHeader.ushKSpaceCentreLineNo = mdh.ushKSpaceCentreLineNo;
             scanhead.scanHeader.ushKSpaceCentrePartitionNo = mdh.ushKSpaceCentrePartitionNo;
             scanhead.scanHeader.sSliceData = mdh.sSliceData;
             memset(scanhead.scanHeader.aushIceProgramPara,0,sizeof(uint16_t)*24);
             memcpy(scanhead.scanHeader.aushIceProgramPara,mdh.aushIceProgramPara,8*sizeof(uint16_t));
             memset(scanhead.scanHeader.aushReservedPara,0,sizeof(uint16_t)*4);
             scanhead.scanHeader.ushApplicationCounter = 0;
             scanhead.scanHeader.ushApplicationMask = 0;
             scanhead.scanHeader.ulCRC = 0;
         }
         else
         {
             f.read(reinterpret_cast<char*>(&scanhead.scanHeader) + sizeof(uint32_t), sizeof(sScanHeader)-sizeof(uint32_t));
         }

         uint32_t dma_length = scanhead.scanHeader.ulFlagsAndDMALength & MDH_DMA_LENGTH_MASK;
         uint32_t mdh_enable_flags = scanhead.scanHeader.ulFlagsAndDMALength & MDH_ENABLE_FLAGS_MASK;


         if (scanhead.scanHeader.aulEvalInfoMask[0] & ( 1 << 5))
         { //Check if this is synch data, if so, it must be handled differently.
             sScanHeader_with_syncdata synch_data;
             synch_data.scanHeader = scanhead.scanHeader;
             synch_data.last_scan_counter = acquisitions-1;

             if (VBFILE)
             {
                 synch_data.syncdata.len = dma_length-sizeof(sMDH);
             }
             else
             {
                 synch_data.syncdata.len = dma_length-sizeof(sScanHeader);
             }
             std::vector<uint8_t> synchdatabytes(synch_data.syncdata.len);
             synch_data.syncdata.p = &synchdatabytes[0];
             f.read(reinterpret_cast<char*>(&synchdatabytes[0]), synch_data.syncdata.len);

             sync_data_packets++;
             continue;
         }

         //This check only makes sense in VD line files.
         if (!VBFILE && (scanhead.scanHeader.lMeasUID != ParcFileEntries[measurement_number-1].measId_))
         {
             //Something must have gone terribly wrong. Bail out.
             if ( first_call )
             {
                 std::cout << "Corrupted or retro-recon dataset detected (scanhead.scanHeader.lMeasUID != ParcFileEntries[" << measurement_number-1 << "].measId_)" << std::endl;
                 std::cout << "Fix the scanhead.scanHeader.lMeasUID ... " << std::endl;
                 first_call = false;
             }
             scanhead.scanHeader.lMeasUID = ParcFileEntries[measurement_number-1].measId_;
         }

         //Allocate data for channels
         scanhead.data.len = scanhead.scanHeader.ushUsedChannels;
         sChannelHeader_with_data* chan = new sChannelHeader_with_data[scanhead.data.len];
         scanhead.data.p = reinterpret_cast<void*>(chan);

         for (unsigned int c = 0; c < scanhead.scanHeader.ushUsedChannels; c++)
         {
             if (VBFILE)
             {
                 if (c > 0)
                 {
                     f.read(reinterpret_cast<char*>(&mdh), sizeof(sMDH));
                 }
                 chan[c].channelHeader.ulTypeAndChannelLength = 0;
                 chan[c].channelHeader.lMeasUID = mdh.lMeasUID;
                 chan[c].channelHeader.ulScanCounter = mdh.ulScanCounter;
                 chan[c].channelHeader.ulReserved1 = 0;
                 chan[c].channelHeader.ulSequenceTime = 0;
                 chan[c].channelHeader.ulUnused2 = 0;
                 chan[c].channelHeader.ulChannelId = mdh.ushChannelId;
                 chan[c].channelHeader.ulUnused3 = 0;
                 chan[c].channelHeader.ulCRC = 0;
             }
             else
             {
                 f.read(reinterpret_cast<char*>(&chan[c].channelHeader), sizeof(sChannelHeader));
             }
             chan[c].data.len = scanhead.scanHeader.ushSamplesInScan;
             chan[c].data.p = reinterpret_cast<void*>(new complex_float_t[chan[c].data.len]);
             f.read(reinterpret_cast<char*>(chan[c].data.p), chan[c].data.len*sizeof(complex_float_t));
         }

         acquisitions++;
         last_mask = scanhead.scanHeader.aulEvalInfoMask[0];

         if (scanhead.scanHeader.aulEvalInfoMask[0] & 1)
         {
             std::cout << "Last scan reached..." << std::endl;
             ClearsScanHeader_with_data(&scanhead);
             break;
         }

         ISMRMRD::Acquisition* ismrmrd_acq = new ISMRMRD::Acquisition;
         // Acquistion header values are zero by default
         ismrmrd_acq->measurement_uid()          = scanhead.scanHeader.lMeasUID;
         ismrmrd_acq->scan_counter()             = scanhead.scanHeader.ulScanCounter;
         ismrmrd_acq->acquisition_time_stamp()   = scanhead.scanHeader.ulTimeStamp;
         ismrmrd_acq->physiology_time_stamp()[0] = scanhead.scanHeader.ulPMUTimeStamp;
         ismrmrd_acq->number_of_samples(scanhead.scanHeader.ushSamplesInScan);
         ismrmrd_acq->available_channels()       = (uint16_t)max_channels;
         ismrmrd_acq->active_channels(scanhead.scanHeader.ushUsedChannels);
         // uint64_t channel_mask[16];     //Mask to indicate which channels are active. Support for 1024 channels
         ismrmrd_acq->discard_pre()             = scanhead.scanHeader.sCutOff.ushPre;
         ismrmrd_acq->discard_post()            = scanhead.scanHeader.sCutOff.ushPost;
         ismrmrd_acq->center_sample()           = scanhead.scanHeader.ushKSpaceCentreColumn;

         if ( scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 25) )
         { //This is noise
             ismrmrd_acq->sample_time_us() =  compute_noise_sample_in_us(ismrmrd_acq->number_of_samples(), isAdjustCoilSens, isVB);
         }
         else
         {
             ismrmrd_acq->sample_time_us() = dwell_time_0 / 1000.0f;
         }

         ismrmrd_acq->position()[0] = scanhead.scanHeader.sSliceData.sSlicePosVec.flSag;
         ismrmrd_acq->position()[1] = scanhead.scanHeader.sSliceData.sSlicePosVec.flCor;
         ismrmrd_acq->position()[2] = scanhead.scanHeader.sSliceData.sSlicePosVec.flTra;

         // Convert Siemens quaternions to direction cosines.
         // In the Siemens convention the quaternion corresponds to a rotation matrix with columns P R S
         // Siemens stores the quaternion as (W,X,Y,Z)
         float quat[4];
         quat[0] = scanhead.scanHeader.sSliceData.aflQuaternion[1]; // X
         quat[1] = scanhead.scanHeader.sSliceData.aflQuaternion[2]; // Y
         quat[2] = scanhead.scanHeader.sSliceData.aflQuaternion[3]; // Z
         quat[3] = scanhead.scanHeader.sSliceData.aflQuaternion[0]; // W
         ISMRMRD::ismrmrd_quaternion_to_directions(  quat,
                                             ismrmrd_acq->phase_dir(),
                                             ismrmrd_acq->read_dir(),
                                             ismrmrd_acq->slice_dir());

         ismrmrd_acq->patient_table_position()[0]  = (float)scanhead.scanHeader.lPTABPosX;
         ismrmrd_acq->patient_table_position()[1]  = (float)scanhead.scanHeader.lPTABPosY;
         ismrmrd_acq->patient_table_position()[2]  = (float)scanhead.scanHeader.lPTABPosZ;

         bool fixedE1E2 = true;
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 25)))   fixedE1E2 = false; // noise
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 1)))    fixedE1E2 = false; // navigator, rt feedback
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 2)))    fixedE1E2 = false; // hp feedback
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 51)))   fixedE1E2 = false; // dummy
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 5)))    fixedE1E2 = false; // synch data

         ismrmrd_acq->idx().average              = scanhead.scanHeader.sLC.ushAcquisition;
         ismrmrd_acq->idx().contrast             = scanhead.scanHeader.sLC.ushEcho;
         ismrmrd_acq->idx().kspace_encode_step_1 = scanhead.scanHeader.sLC.ushLine;
         ismrmrd_acq->idx().kspace_encode_step_2 = scanhead.scanHeader.sLC.ushPartition;
         ismrmrd_acq->idx().phase                = scanhead.scanHeader.sLC.ushPhase;
         ismrmrd_acq->idx().repetition           = scanhead.scanHeader.sLC.ushRepetition;
         ismrmrd_acq->idx().segment              = scanhead.scanHeader.sLC.ushSeg;
         ismrmrd_acq->idx().set                  = scanhead.scanHeader.sLC.ushSet;
         ismrmrd_acq->idx().slice                = scanhead.scanHeader.sLC.ushSlice;
         ismrmrd_acq->idx().user[0]            = scanhead.scanHeader.sLC.ushIda;
         ismrmrd_acq->idx().user[1]            = scanhead.scanHeader.sLC.ushIdb;
         ismrmrd_acq->idx().user[2]            = scanhead.scanHeader.sLC.ushIdc;
         ismrmrd_acq->idx().user[3]            = scanhead.scanHeader.sLC.ushIdd;
         ismrmrd_acq->idx().user[4]            = scanhead.scanHeader.sLC.ushIde;
         // TODO: remove this once the GTPlus can properly autodetect partial fourier
         ismrmrd_acq->idx().user[5]            = scanhead.scanHeader.ushKSpaceCentreLineNo;
         ismrmrd_acq->idx().user[6]            = scanhead.scanHeader.ushKSpaceCentrePartitionNo;

         /*****************************************************************************/
         /* the user_int[0] and user_int[1] are used to store user defined parameters */
         /*****************************************************************************/
         ismrmrd_acq->user_int()[0]   = scanhead.scanHeader.aushIceProgramPara[0];
         ismrmrd_acq->user_int()[1]   = scanhead.scanHeader.aushIceProgramPara[1];
         ismrmrd_acq->user_int()[2]   = scanhead.scanHeader.aushIceProgramPara[2];
         ismrmrd_acq->user_int()[3]   = scanhead.scanHeader.aushIceProgramPara[3];
         ismrmrd_acq->user_int()[4]   = scanhead.scanHeader.aushIceProgramPara[4];
         ismrmrd_acq->user_int()[5]   = scanhead.scanHeader.aushIceProgramPara[5];
         ismrmrd_acq->user_int()[6]   = scanhead.scanHeader.aushIceProgramPara[6];
         ismrmrd_acq->user_int()[7]   = scanhead.scanHeader.aushIceProgramPara[7];

         ismrmrd_acq->user_float()[0] = scanhead.scanHeader.aushIceProgramPara[8];
         ismrmrd_acq->user_float()[1] = scanhead.scanHeader.aushIceProgramPara[9];
         ismrmrd_acq->user_float()[2] = scanhead.scanHeader.aushIceProgramPara[10];
         ismrmrd_acq->user_float()[3] = scanhead.scanHeader.aushIceProgramPara[11];
         ismrmrd_acq->user_float()[4] = scanhead.scanHeader.aushIceProgramPara[12];
         ismrmrd_acq->user_float()[5] = scanhead.scanHeader.aushIceProgramPara[13];
         ismrmrd_acq->user_float()[6] = scanhead.scanHeader.aushIceProgramPara[14];
         ismrmrd_acq->user_float()[7] = scanhead.scanHeader.aushIceProgramPara[15];

         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 25)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_NOISE_MEASUREMENT);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 28)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_FIRST_IN_SLICE);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 29)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_LAST_IN_SLICE);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 11)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_LAST_IN_REPETITION);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 22)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_PARALLEL_CALIBRATION);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 23)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_PARALLEL_CALIBRATION_AND_IMAGING);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 24)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_REVERSE);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 11)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_LAST_IN_MEASUREMENT);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 21)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_PHASECORR_DATA);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 1)))    ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_NAVIGATION_DATA);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 1)))    ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_RTFEEDBACK_DATA);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 2)))    ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_HPFEEDBACK_DATA);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 51)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_DUMMYSCAN_DATA);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 10)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_SURFACECOILCORRECTIONSCAN_DATA);
         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 5)))    ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_IS_DUMMYSCAN_DATA);
         // if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 1))) ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_LAST_IN_REPETITION);

         if ((scanhead.scanHeader.aulEvalInfoMask[0] & (1ULL << 46)))   ismrmrd_acq->setFlag(ISMRMRD::ISMRMRD_ACQ_LAST_IN_MEASUREMENT);

         if ((flash_pat_ref_scan) & (ismrmrd_acq->isFlagSet(ISMRMRD::ISMRMRD_ACQ_IS_PARALLEL_CALIBRATION)))
         {
             // For some sequences the PAT Reference data is collected using a different encoding space
             // e.g. EPI scans with FLASH PAT Reference
             // enabled by command line option
             // TODO: it is likely that the dwell time is not set properly for this type of acquisition
             ismrmrd_acq->encoding_space_ref() = 1;
         }

         if (trajectory == 4)
         { //Spiral, we will add the trajectory to the data

             // from above we have the following
             // traj_dim[0] = dimensionality (2)
             // traj_dim[1] = ngrad i.e. points per interleaf
             // traj_dim[2] = no. of interleaves
             // and
             // traj.getData() is a float * pointer to the trajectory stored
             // kspace_encode_step_1 is the interleaf number

             if (!(ismrmrd_acq->isFlagSet(ISMRMRD::ISMRMRD_ACQ_IS_NOISE_MEASUREMENT)))
             { //Only when this is not noise
                  unsigned long traj_samples_to_copy = ismrmrd_acq->number_of_samples();
                  if (traj_dim[1] < traj_samples_to_copy)
                  {
                      traj_samples_to_copy = (unsigned long)traj_dim[1];
                      ismrmrd_acq->discard_post() = (uint16_t)(ismrmrd_acq->number_of_samples()-traj_samples_to_copy);
                  }
                  // Set the trajectory dimensions
                  // this reallocates the memory for the trajectory
                  ismrmrd_acq->trajectory_dimensions(traj_dim[0]);
                  float* t_ptr = &traj.getData()[ traj_dim[0] * traj_dim[1] * ismrmrd_acq->idx().kspace_encode_step_1 ];
                  memcpy(ismrmrd_acq->getTraj(), t_ptr, sizeof(float) * traj_dim[0] * traj_samples_to_copy);
             }
         }

         sChannelHeader_with_data* channel_header = reinterpret_cast<sChannelHeader_with_data*>(scanhead.data.p);
         for (unsigned int c = 0; c < ismrmrd_acq->active_channels(); c++)
         {
             complex_float_t* dptr = static_cast< complex_float_t* >(channel_header[c].data.p);
             memcpy(&(static_cast<complex_float_t*>(ismrmrd_acq->getData())[c*ismrmrd_acq->number_of_samples()]), dptr, ismrmrd_acq->number_of_samples()*sizeof(complex_float_t));
         }

         ismrmrd_dataset.appendAcquisition(*ismrmrd_acq);

         if ( scanhead.scanHeader.ulScanCounter % 1000 == 0 ) {
             std::cout << "wrote scan : " << scanhead.scanHeader.ulScanCounter << std::endl;
         }

         {
             ClearsScanHeader_with_data(&scanhead);
         }

         delete ismrmrd_acq;
         
     }//End of the while loop

     //Mystery bytes. There seems to be 160 mystery bytes at the end of the data.
     unsigned int mystery_bytes = (ParcFileEntries[measurement_number-1].off_+ParcFileEntries[measurement_number-1].len_)-f.tellg();

     if (mystery_bytes > 0)
     {
         if (mystery_bytes != 160)
         {
             //Something in not quite right
             std::cout << "WARNING: Unexpected number of mystery bytes detected: " << mystery_bytes << std::endl;
             std::cout << "ParcFileEntries[" << measurement_number-1 << "].off_ = " << ParcFileEntries[measurement_number-1].off_ << std::endl;
             std::cout << "ParcFileEntries[" << measurement_number-1 << "].len_ = " << ParcFileEntries[measurement_number-1].len_ << std::endl;
             std::cout << "f.tellg() = " << f.tellg() << std::endl;
             std::cout << "Please check the result." << std::endl;
         }
         else
         {
             //Read the mystery bytes
             f.read(reinterpret_cast<char*>(&mystery_data), mystery_bytes);
             //After this we have to be on a 512 byte boundary
             if (f.tellg() % 512)
             {
                 f.seekg(512-(f.tellg() % 512), std::ios::cur);
             }
         }
     }

     size_t end_position = f.tellg();
     f.seekg(0,std::ios::end);
     size_t eof_position = f.tellg();
     if (end_position != eof_position && ParcRaidHead.count_ == measurement_number)
     {
         size_t additional_bytes = eof_position-end_position;
         std::cout << "WARNING: End of file was not reached during conversion. There are " << additional_bytes << " additional bytes at the end of file." << std::endl;
     }

     ismrmrd_dataset.close();
     f.close();

     return 0;
}
