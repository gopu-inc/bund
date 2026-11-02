import re
from pathlib import Path

from setuptools import find_packages, setup

here = Path(__file__).parent
version = re.search(r'VERSION\s*=\s*"([^"]+)"', (here / "bund" / "config.py").read_text(encoding="utf-8")).group(1)

setup(
    name="bund",
    version=version,
    description="BUND — agent de code autonome en terminal (Textual + asyncio), façon IDE",
    long_description=(here / "README.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    author="Bund",
    url="https://github.com/gopu-inc/bund",
    packages=find_packages(include=["bund", "bund.*"]),
    python_requires=">=3.9",
    install_requires=["textual>=0.80"],
    extras_require={"web": ["streamlit>=1.30"]},
    entry_points={"console_scripts": ["bund=bund.cli:main"]},
)
