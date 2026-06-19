"""
Victor 9000 File Transfer Library - Python Package Setup.
"""

from setuptools import setup, find_packages

setup(
    name="v9kfiletrx",
    version="1.0.0",
    description="File transfer library for Victor 9000",
    author="Victor 9000 Development",
    packages=find_packages(),
    python_requires=">=3.9",
    install_requires=[
        # Requires v9kserial package from ../serial/python
    ],
    extras_require={
        "dev": [
            "pytest>=7.0",
            "pytest-cov>=4.0",
        ],
    },
    entry_points={
        "console_scripts": [
            "v9k-send=v9kfiletrx.cli:send_main",
            "v9k-recv=v9kfiletrx.cli:recv_main",
        ],
    },
)
