#RFC 4287
#python atomtools.py copiasample.atom

#From 1.1 of the spec
ATOM_MODEL = '''<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"
  xmlns:atom="http://www.w3.org/2005/Atom"
  xmlns:eg="http://examplotron.org/0/"
  xmlns:ak="http://purl.org/dc/org/xml3k/akara"
  ak:resource="atom:id"
 >

 <title type="text" ak:rel="local-name()" ak:value=".">A title</title>
 <subtitle type="text" ak:rel="local-name()" ak:value=".">A subtitle</subtitle>
 <updated ak:rel="local-name()" ak:value=".">2003-12-13T18:30:02Z</updated>
 <author eg:occurs="*" ak:resource="(atom:uri|atom:email)[1]" ak:rel="local-name()">
   <name ak:rel="local-name()" ak:value=".">John Doe</name>
   <uri ak:rel="local-name()" ak:value=".">http://example.org/</uri>
   <email ak:rel="local-name()" ak:value=".">jdjd@example.com</email>
 </author>
 <id ak:rel="local-name()" ak:value=".">urn:uuid:60a76c80-d399-11d9-b93C-0003939e0af6</id>
 <link rel="self" type="application/atom+xml"
   href="http://example.org/feed.atom" eg:occurs="*" ak:rel="local-name()" ak:value="@href"/>
 <rights ak:rel="local-name()" ak:value=".">Copyright (c) 2003, John Doe</rights>
 <generator uri="http://www.example.com/" version="1.0">
   Example Toolkit
 </generator>

 <entry eg:occurs="*" ak:resource="atom:id" ak:rel="local-name()">
   <title ak:rel="local-name()" ak:value=".">Atom-Powered Robots Run Amok</title>
   <link rel="self" type="application/atom+xml" hreflang="en"
     href="http://example.org/2003/12/13/atom03" eg:occurs="*" ak:rel="local-name()" ak:value="@href"/>
   <id ak:rel="local-name()" ak:value=".">tag:example.org,2003:3</id>
   <updated ak:rel="local-name()" ak:value=".">2003-12-13T18:30:02Z</updated>
   <published ak:rel="local-name()" ak:value=".">2003-12-13T08:29:29-04:00</published>
   <summary type="text" ak:rel="local-name()" ak:value=".">Some text.</summary>
   <author eg:occurs="*" ak:resource="(atom:uri|atom:email)[1]" ak:rel="local-name()">
     <name ak:rel="local-name()" ak:value=".">John Doe</name>
     <uri ak:rel="local-name()" ak:value=".">http://example.org/</uri>
     <email ak:rel="local-name()" ak:value=".">jdjd@example.com</email>
   </author>
   <contributor eg:occurs="*" ak:resource="(atom:uri|atom:email)[1]" ak:rel="local-name()">
     <name ak:rel="local-name()" ak:value=".">Jane Doe</name>
     <uri ak:rel="local-name()" ak:value=".">http://example.org/Jane</uri>
     <email ak:rel="local-name()" ak:value=".">janed@example.com</email>
   </contributor>
   <content type="text" xml:lang="en" xml:base="http://example.org/" ak:rel="local-name()" ak:value=".">
     The main ingredient
   </content>
 </entry>

</feed>'''

import sys
from amara.bindery.model import *
from amara import bindery
from itertools import *
from operator import *

MODEL = examplotron_model(ATOM_MODEL)


import re, copy
from cStringIO import StringIO
from datetime import datetime

import amara

SLUGCHARS = r'a-zA-Z0-9\-\_'
OMIT_FROM_SLUG_PAT = re.compile('[^%s]'%SLUGCHARS)

TYPE = 'type'
UPDATED = 'updated'
TITLE = 'title'
ID = 'id'

ATOM_MT = u'application/atom+xml'

slug_from_title = lambda t: OMIT_FROM_SLUG_PAT.sub('_', t).lower().decode('utf-8')[:20]

datetime_from_iso = lambda ds: datetime.strptime(ds, "%Y-%m-%dT%H:%M:%SZ")

path_from_datetime = lambda dt: '%i/%i'%dt.utctimetuple()[:2]

#def create_entry(doc):
#    for entry in doc.entry

def atomindex(entry):
    yield TYPE, ATOM_MT
    yield TITLE, unicode(entry.title)
    yield ID, unicode(entry.id)
    yield UPDATED, unicode(entry.updated)
    #yield TITLE, doc.xml_select(u'name(*)')


from amara.namespaces import *
from amara.bindery import html

NSS = {COMMON_PREFIXES[ATOM_NAMESPACE]: ATOM_NAMESPACE}

def tidy_content_element(root, check=u'//atom:title|//atom:summary|//atom:content', prefixes=NSS):
    """
    Takes all Atom content elements with type=html (i.e. a:title, a:summary or a:content)
    And convert them to be of type=xhtml

    This operation mutates root in place.

    Example:

    import amara; from util import tidy_content_element
    A = '<entry xmlns="http://www.w3.org/2005/Atom"><id>urn:bogus:x</id><title type="html">&lt;div&gt;x&lt;p&gt;y&lt;p&gt;&lt;/div&gt;</title></entry>'
    doc = amara.parse(A)
    tidy_content_element(doc)
    amara.xml_print(doc)
    """
    nodes = root.xml_select(check, prefixes)
    for node in nodes:
        if node.xml_select(u'@type = "html"') and node.xml_select(u'string(.)'):
            unsouped = html.parse('<html>%s</html>'%node.xml_select(u'string(.)').encode('utf-8'))
            #amara.xml_print(unsouped, stream=sys.stderr)
            while node.xml_children: node.xml_remove(node.xml_first_child)
            newcontent = '<div xmlns="http://www.w3.org/1999/xhtml">'
            for child in unsouped.html.body.xml_children:
                s = StringIO()
                amara.xml_print(child, stream=s)
                newcontent += s.getvalue()
            newcontent += '</div>'
            node.xml_append(amara.parse(newcontent).xml_first_child)
            node.xml_attributes[None, u'type'] = u'xhtml'
            #for child in doc.html.body.xml_children:
            #    div.xml_append(child)
    return root

    

'''
fname = tempfile.mktemp('.xml')
driver.init_db(sqlite3.connect(fname))
drv = driver(sqlite3.connect(fname))
content = MONTY_XML
id = drv.create_resource(StringIO(content), metadata=dict(myindex(content)))
content1, metadata = drv.get_resource(id)
content1 = content1.read()
doc = amara.parse(content)
self.assertEqual(content, content1)
self.assertEqual(metadata[u'root-element-name'], doc.xml_select(u'name(*)'))
self.assertEqual(metadata[u'element-count'], doc.xml_select(u'count(//*)'))
return
'''



def run(source, normalize):
    doc = bindery.parse(source, model=MODEL)
    #print doc.labels.xml_model.generate_metadata(doc)
    #import pprint
    #pprint.pprint(doc.feed.xml_model.generate_metadata(doc))
    metadata = doc.feed.xml_model.generate_metadata(doc)
    raw_feeddata = {}
    for eid, row in groupby(sorted(metadata, key=itemgetter(0)), itemgetter(0)):
        entity = {}
        for r in row:
            entity.setdefault(r[1], []).append(r[2])
        raw_feeddata[eid] = entity
    import pprint
    pprint.pprint(raw_feeddata)
    feeddata = {}
    return


#Ideas borrowed from
# http://www.artima.com/forums/flat.jsp?forum=106&thread=4829

def command_line_prep():
    from optparse import OptionParser
    usage = "%prog [options] source cmd"
    parser = OptionParser(usage=usage)
    parser.add_option("-n", "--normalize",
                      action="store_false", dest="normalize", default=-False,
                      help="send a normalized version of the Atom to the console")
    #parser.add_option("-q", "--quiet",
    #                  action="store_false", dest="verbose", default=1,
    #                  help="don't print status messages to stdout", metavar="<PREFIX=URI>")
    return parser


def main(argv=None):
    #But with better integration of entry points
    if argv is None:
        argv = sys.argv
    # By default, optparse usage errors are terminated by SystemExit
    try:
        optparser = command_line_prep()
        options, args = optparser.parse_args(argv[1:])
        # Process mandatory arguments with IndexError try...except blocks
        try:
            source = args[0]
        except IndexError:
            optparser.error("Missing filename/URL to parse")
        #try:
        #    xpattern = args[1]
        #except IndexError:
        #    optparser.error("Missing main xpattern")
    except SystemExit, status:
        return status

    # Perform additional setup work here before dispatching to run()
    # Detectable errors encountered here should be handled and a status
    # code of 1 should be returned. Note, this would be the default code
    # for a SystemExit exception with a string message.

    #try:
    #    xpath = args[2].decode('utf-8')
    #except IndexError:
    #    xpath = None
    #xpattern = xpattern.decode('utf-8')
    #sentinel = options.sentinel and options.sentinel.decode('utf-8')
    #display = options.display and options.display.decode('utf-8')
    #prefixes = options.ns
    #limit = options.limit
    #if source == '-':
    #    source = sys.stdin
    normalize = options.normalize
    #run(source, xpattern, xpath, limit, sentinel, display, prefixes)
    run(source, normalize)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

