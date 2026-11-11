# Final prefix and cleanup

Authored anonymous constructor regression with final stores before split
catch-all regions. The original constructor boundary, stores, cleanup, abrupt
exits and partially initialized objects must remain observable. All generated
Java and original metadata are checked, with source and external context modes.
