from setuptools import setup, find_packages

setup(
    name="milansql",
    version="1.7.0",
    description="Python client for MilanSQL database",
    long_description=open("README.md", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    author="Mirwais Haidari",
    url="https://github.com/haidari9819-lang/milansql",
    packages=find_packages(),
    python_requires=">=3.8",
    install_requires=[],      # stdlib only: socket, json, urllib
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
        "Topic :: Database",
        "Topic :: Database :: Front-Ends",
    ],
    keywords="milansql database client sql tcp",
)
