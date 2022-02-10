PY23_LIBRARY()

LICENSE(MIT)

OWNER(g:python-contrib)

VERSION(0.18.2)

NO_CHECK_IMPORTS(
    future.backports.email.policy  # email backport is incomplete in v0.16.0.
    future.moves.dbm.ndbm
)

NO_LINT()
NO_EXTENDED_SOURCE_SEARCH()

PY_SRCS(
    TOP_LEVEL
    future/backports/__init__.py
    future/backports/_markupbase.py
    future/backports/datetime.py
    future/backports/email/__init__.py
    future/backports/email/_encoded_words.py
    future/backports/email/_header_value_parser.py
    future/backports/email/_parseaddr.py
    future/backports/email/_policybase.py
    future/backports/email/base64mime.py
    future/backports/email/charset.py
    future/backports/email/encoders.py
    future/backports/email/errors.py
    future/backports/email/feedparser.py
    future/backports/email/generator.py
    future/backports/email/header.py
    future/backports/email/headerregistry.py
    future/backports/email/iterators.py
    future/backports/email/message.py
    future/backports/email/mime/__init__.py
    future/backports/email/mime/application.py
    future/backports/email/mime/audio.py
    future/backports/email/mime/base.py
    future/backports/email/mime/image.py
    future/backports/email/mime/message.py
    future/backports/email/mime/multipart.py
    future/backports/email/mime/nonmultipart.py
    future/backports/email/mime/text.py
    future/backports/email/parser.py
    future/backports/email/policy.py
    future/backports/email/quoprimime.py
    future/backports/email/utils.py
    future/backports/html/__init__.py
    future/backports/html/entities.py
    future/backports/html/parser.py
    future/backports/http/__init__.py
    future/backports/http/client.py
    future/backports/http/cookiejar.py
    future/backports/http/cookies.py
    future/backports/http/server.py
    future/backports/misc.py
    future/backports/socket.py
    future/backports/socketserver.py
    future/backports/total_ordering.py
    future/backports/urllib/__init__.py
    future/backports/urllib/error.py
    future/backports/urllib/parse.py
    future/backports/urllib/request.py
    future/backports/urllib/response.py
    future/backports/urllib/robotparser.py
    future/backports/xmlrpc/__init__.py
    future/backports/xmlrpc/client.py
    future/backports/xmlrpc/server.py
    future/builtins/__init__.py
    future/builtins/disabled.py
    future/builtins/iterators.py
    future/builtins/misc.py
    future/builtins/new_min_max.py
    future/builtins/newnext.py
    future/builtins/newround.py
    future/builtins/newsuper.py
    future/moves/__init__.py
    future/moves/_dummy_thread.py
    future/moves/_markupbase.py
    future/moves/_thread.py
    future/moves/builtins.py
    future/moves/collections.py
    future/moves/configparser.py
    future/moves/copyreg.py
    future/moves/dbm/__init__.py
    future/moves/dbm/dumb.py
    future/moves/dbm/ndbm.py
    future/moves/html/__init__.py
    future/moves/html/entities.py
    future/moves/html/parser.py
    future/moves/http/__init__.py
    future/moves/http/client.py
    future/moves/http/cookiejar.py
    future/moves/http/cookies.py
    future/moves/http/server.py
    future/moves/itertools.py
    future/moves/pickle.py
    future/moves/queue.py
    future/moves/reprlib.py
    future/moves/socketserver.py
    future/moves/subprocess.py
    future/moves/sys.py
    future/moves/urllib/__init__.py
    future/moves/urllib/error.py
    future/moves/urllib/parse.py
    future/moves/urllib/request.py
    future/moves/urllib/response.py
    future/moves/urllib/robotparser.py
    future/moves/xmlrpc/__init__.py
    future/moves/xmlrpc/client.py
    future/moves/xmlrpc/server.py
    future/standard_library/__init__.py
    future/tests/__init__.py
    future/tests/base.py
    future/types/__init__.py
    future/types/newbytes.py
    future/types/newdict.py
    future/types/newint.py
    future/types/newlist.py
    future/types/newmemoryview.py
    future/types/newobject.py
    future/types/newopen.py
    future/types/newrange.py
    future/types/newstr.py
    future/utils/__init__.py
    future/utils/surrogateescape.py
    past/builtins/__init__.py 
    past/builtins/misc.py 
    past/builtins/noniterators.py 
    past/tests/__init__.py 
    past/types/__init__.py 
    past/types/basestring.py 
    past/types/olddict.py 
    past/types/oldstr.py 
    past/utils/__init__.py 
)

IF (MODULE_TAG == "PY2")
    PY_SRCS(
        TOP_LEVEL
        _dummy_thread/__init__.py
        _markupbase/__init__.py
        _thread/__init__.py
        builtins/__init__.py
        copyreg/__init__.py
        future/__init__.py
        html/__init__.py
        html/entities.py
        html/parser.py
        http/__init__.py
        http/client.py
        http/cookiejar.py
        http/cookies.py
        http/server.py
        past/__init__.py 
        queue/__init__.py
        reprlib/__init__.py
        socketserver/__init__.py
        xmlrpc/__init__.py
        xmlrpc/client.py
        xmlrpc/server.py
    )
ENDIF()

IF (OS_WINDOWS)
    PY_SRCS(
        TOP_LEVEL
        future/moves/winreg.py
        winreg/__init__.py
    )
ENDIF()

END()
