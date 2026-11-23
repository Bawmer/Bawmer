# Third-party notice

The files in this folder are the model definition of RVC v2
(Retrieval-based-Voice-Conversion-WebUI, https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI,
MIT License, Copyright (c) 2023 liujing04 and contributors) as packaged by
rvc-python 0.1.5 (https://pypi.org/project/rvc-python/, MIT License, Copyright (c) 2023 Danil Boldyrev).
Only the import statements were changed.

The pretrained base weights (`f0G48k.pth`, `f0D48k.pth`) are **not** stored in this repository.
`voxforge-engine download-base` fetches them from the RVC project's model repository
(https://huggingface.co/lj1995/VoiceConversionWebUI, MIT License). According to the RVC project they were
trained on roughly 50 hours of the open VCTK corpus (110 speakers).
