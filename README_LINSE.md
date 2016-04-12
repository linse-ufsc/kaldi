LINSE-specific instructions
===========================

The *master* branch **MUST** always be mergeable with the upstream Kaldi project (i.e. don't do any development in this branch).

The *training* branch contains changes to the upstream Kaldi project that are necessary for the acoustic model setup in LINSE (mainly a faster way to read files via HTTP using libcurl).
