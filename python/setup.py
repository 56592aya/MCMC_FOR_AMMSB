from distutils.core import setup
from Cython.Build import cythonize
from distutils.extension import Extension

extensions = (
    "com/uva/*.pyx",
    Extension(
        "com/uva/custom_random/custom_random",
        ["com/uva/custom_random/custom_random.pyx"],
        include_dirs=["../c++/src", "../build-compat/src"],
        library_dirs=["../build-compat/src"],
        libraries=["mcmc"],
        language="c++",
        extra_compile_args=["-std=c++11"],
        ),                                 # generate C++ code
    )

setup(
    ext_modules = cythonize(extensions)
)
