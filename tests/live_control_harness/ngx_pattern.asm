.code
NgxSignature PROC
    db 084h, 0D2h, 00Fh, 084h, 003h, 001h, 000h, 000h
    db 0BEh, 005h, 000h, 000h, 000h
    ret
NgxSignature ENDP

NgxSynthesisSignature PROC
    db 03Dh, 0B0h, 001h, 000h, 000h
    db 00Fh, 093h, 0C0h
    db 088h, 047h, 028h
    db 040h, 088h, 077h, 029h
NgxSynthesisSignature ENDP

NgxMultiFrameMaximumSignature PROC
    db 081h, 0FDh, 0B0h, 001h, 000h, 000h
    db 00Fh, 08Ch, 09Fh, 000h, 000h, 000h
    db 0BFh, 005h, 000h, 000h, 000h
    ret
NgxMultiFrameMaximumSignature ENDP
END
