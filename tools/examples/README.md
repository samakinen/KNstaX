# `.knxprod` Example Flow

This folder contains a small extracted KNX product XML example used to validate the importer-driven profile generation flow.

Generate the profile header:

```bash
python3 tools/knxprod2profile.py tools/examples/sample.knxprod.xml --output build/generated_product_profile.hpp
```

Compile-check the generated header:

```bash
python3 tools/knxprod2profile_check.py build/generated_product_profile.hpp
```

Run the smoke test for the sample:

```bash
python3 tools/knxprod2profile_smoke_test.py
```