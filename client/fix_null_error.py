import os

files = [
    r'd:\Qt\QQQQt\programlzz\MYMQ\client\src\MYMQ_Client.cpp',
    r'd:\Qt\QQQQt\programlzz\MYMQ\client\src\MYMQ_Client.h',
    r'd:\Qt\QQQQt\programlzz\MYMQ\client\src\ClientProtocol.cpp',
    r'd:\Qt\QQQQt\programlzz\MYMQ\client\examples\main.cpp',
    r'd:\Qt\QQQQt\programlzz\MYMQ\client\examples\example_test_perf.cpp',
    r'd:\Qt\QQQQt\programlzz\MYMQ\client\examples\example_test_seek.cpp'
]

for file_path in files:
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        new_content = content.replace('NULL_ERROR', 'Success')
        
        if new_content != content:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(new_content)
            print(f"Updated {file_path}")
        else:
            print(f"No changes needed for {file_path}")
    except Exception as e:
        print(f"Error processing {file_path}: {e}")
