# Regenerate the IRT fixtures from the datasets bundled with R's mirt package,
# which tests/lib/159_irt.telic fits and checks against mirt's own estimates:
#   data/lsat7.tsv    LSAT7, 1,000 people x 5 right/wrong items (Bock and
#                     Lieberman, 1970), expanded from mirt's frequency table
#   data/science.tsv  Science, 392 people x 4 items on a four-point scale
#                     (Karlheinz and Melich, 1992)
#
#   Rscript tools/gen-irt-fixtures.R
suppressMessages(library(mirt))
write.table(expand.table(LSAT7), "data/lsat7.tsv", sep = "\t", quote = FALSE, row.names = FALSE)
write.table(Science, "data/science.tsv", sep = "\t", quote = FALSE, row.names = FALSE)
