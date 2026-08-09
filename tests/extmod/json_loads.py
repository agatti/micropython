try:
    import json
except ImportError:
    print("SKIP")
    raise SystemExit


def my_print(o):
    if isinstance(o, dict):
        print("sorted dict", sorted(o.items()))
    else:
        print(o)


my_print(json.loads("null"))
my_print(json.loads("false"))
my_print(json.loads("true"))
my_print(json.loads("1"))
my_print(json.loads("-2"))
my_print(json.loads('"abc\\u0064e"'))
my_print(json.loads("[]"))
my_print(json.loads("[null]"))
my_print(json.loads("[null,false,true]"))
my_print(json.loads(" [ null , false , true ] "))
my_print(json.loads("{}"))
my_print(json.loads('{"a":true}'))
my_print(json.loads('{"a":null, "b":false, "c":true}'))
my_print(json.loads('{"a":[], "b":[1], "c":{"3":4}}'))
my_print(json.loads('"abc\\bdef"'))
my_print(json.loads('"abc\\fdef"'))
my_print(json.loads('"abc\\ndef"'))
my_print(json.loads('"abc\\rdef"'))
my_print(json.loads('"abc\\tdef"'))
my_print(json.loads('"abc\\uabcd"'))
my_print(json.loads('"\\""'))
my_print(json.loads('"\\/"'))

# whitespace handling
my_print(json.loads('{\n\t"a":[]\r\n, "b":[1], "c":{"3":4}     \n\r\t\r\r\r\n}'))

# loading nothing should raise exception
try:
    json.loads("")
except ValueError:
    print("ValueError")

# string which is not closed
try:
    my_print(json.loads('"abc'))
except ValueError:
    print("ValueError")

# unaccompanied closing brace
try:
    my_print(json.loads("]"))
except ValueError:
    print("ValueError")

# unspecified object type
try:
    my_print(json.loads("a"))
except ValueError:
    print("ValueError")

# bad property name
try:
    my_print(json.loads('{{}:"abc"}'))
except ValueError:
    print("ValueError")

# unexpected characters after white space
try:
    my_print(json.loads("[null]   a"))
except ValueError:
    print("ValueError")

# incomplete object declaration
try:
    my_print(json.loads('{"a":0,'))
except ValueError:
    print("ValueError")

# incomplete nested array declaration
try:
    my_print(json.loads('{"a":0, ['))
except ValueError:
    print("ValueError")

# incomplete array declaration
try:
    my_print(json.loads("[0,"))
except ValueError:
    print("ValueError")

# incomplete nested object declaration
try:
    my_print(json.loads('[0, {"a":0, '))
except ValueError:
    print("ValueError")

# incorrect base 10 number representation
try:
    my_print(json.loads("[-01]"))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("[-012]"))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("[012]"))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("[+012]"))
except ValueError:
    print("ValueError")

# non-string object keys
try:
    my_print(json.loads("{1:1}"))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads('{1:"1"}'))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("{null:null}"))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads('{null: "value"}'))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("{9999E9999:1}"))
except ValueError:
    print("ValueError")

# unsupported string escape
try:
    my_print(json.loads('"\\a"'))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads('"\\z"'))
except ValueError:
    print("ValueError")

# dict pair with key and no value
try:
    my_print(json.loads('{"key":}'))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads('{"key":"value", "key2":}'))
except ValueError:
    print("ValueError")

# multiple element separators
try:
    my_print(json.loads('{"a"::"b"}'))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("[1,,2]"))
except ValueError:
    print("ValueError")
try:
    my_print(json.loads("[1,:2]"))
except ValueError:
    print("ValueError")
