import unittest
import os
from cStringIO import StringIO
import tempfile

from amara.lib import testsupport
from amara.bindery import html
from amara import tree, xml_print, bindery
from amara import tree

from amara.lib.treecompare import xml_compare

from amara.tools.atomtools import *

from amara.test import test_main
from amara.test.xslt import filesource, stringsource

def tidy_atom(source):
    doc = bindery.parse(source.source)
    tidy_content_element(doc)
    buf = StringIO()
    xml_print(doc, stream=buf, indent=True)
    #self.assertEqual(buf.getvalue(), expected)
    return buf.getvalue()


class Test_tidy_atom(unittest.TestCase):
    """Testing untidy atom 1"""
    def test_parse_file(self):
        doc = bindery.parse(tidy_atom(filesource('entry1.atom')))
        self.assertEqual(len(doc.xml_children), 1)
        self.assertEqual(len(list(doc.entry)), 1)
        self.assertEqual(len(list(doc.entry.link)), 1)
        return


#
class Test_ejsonize(unittest.TestCase):
    """Testing conversion to simple data structure"""
    def test_ejsonize1(self):
        EXPECTED = [{u'updated': u'2005-07-31T12:29:29Z', u'title': u'Atom draft-07 snapshot', u'label': u'tag:example.org,2003:3.2397', u'content_text': u'\n     \n       [Update: The Atom draft is finished.]\n     \n   ', u'link': u'http://example.org/2005/04/02/atom', u'authors': [u'Mark Pilgrim'], u'summary': u'None', u'type': u'Entry'}]
        results = ejsonize(filesource('rfc4287-1-1-2.atom').source)
        self.assertEqual(results, EXPECTED)
        return

class Test_feed(unittest.TestCase):
    """Test specialized feed bindery class, atomtools.feed"""
    SKEL = """<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>Test feed</title>
  <id>http://example.org/CHANGE_ME</id>
  <!--updated>2009-03-03T11:50:21Z</updated-->

</feed>"""

    EXPECTED_RSS1 = """<rdf:RDF xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:content="http://purl.org/rss/1.0/modules/content/" xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <channel xmlns="http://purl.org/rss/1.0/" rdf:about="http://example.org/">
    <title>dive into mark</title>
    <description/>
    <link>http://example.org/</link>
    <items>
      <rdf:Seq>
        <rdf:li>http://example.org/2005/04/02/atom</rdf:li>
      </rdf:Seq>
    </items>
  </channel>
  <item xmlns="http://purl.org/rss/1.0/" rdf:about="http://example.org/2005/04/02/atom">
    <title>Atom draft-07 snapshot</title>
    <description>
     
       
          [Update: The Atom draft is finished.]
        
     
   </description>
    <content:encoded>
       &lt;p&gt;
          &lt;i&gt;[Update: The Atom draft is finished.]&lt;/i&gt;
        &lt;/p&gt;
     </content:encoded>
    <dc:date>2005-07-31T12:29:29Z</dc:date>
    <link>http://example.org/2005/04/02/atom</link>
  </item>
</rdf:RDF>"""

    #
    def test_init1(self):
        EXPECTED_TITLE = u'Test feed'
        f = feed(skel=self.SKEL)
        self.assertEqual(unicode(f.feed.title), EXPECTED_TITLE)
        return

    def test_init2(self):
        EXPECTED_TITLE = u'Sample feed'
        f = feed()
        self.assertEqual(unicode(f.feed.title), EXPECTED_TITLE)
        return

    def test_entry1(self):
        EXPECTED_TITLE = u'Test entry'
        EXPECTED_ID = u'urn:bogus:spam1'
        f = feed(skel=self.SKEL)
        author = (u'Uche Ogbuji', u'Uche@Ogbuji.net', u'http://Uche.Ogbuji.net')
        f.append('urn:bogus:spam1', u"Test entry", summary=u"Summary", content=u"Contento", authors=[author])
        self.assertEqual(unicode(f.feed.entry.title), EXPECTED_TITLE)
        self.assertEqual(unicode(f.feed.entry.id), EXPECTED_ID)
        return

    def test_atom2rdf_rss(self):
        """Testing atom to RSS conversion"""
        EXPECTED_TITLE = u'dive into mark'
        f = feed(feedxml=tidy_atom(filesource('rfc4287-1-1-2.atom')))
        self.assertEqual(unicode(f.feed.title), EXPECTED_TITLE)
        self.assert_(xml_compare(f.rss1format(), self.EXPECTED_RSS1))
        return

    

if __name__ == '__main__':
    testsupport.test_main()

