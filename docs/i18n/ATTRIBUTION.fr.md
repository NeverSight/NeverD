**Langues**: [English](../../ATTRIBUTION.md) | [简体中文](ATTRIBUTION.zh-CN.md) | [繁體中文](ATTRIBUTION.zh-TW.md) | [日本語](ATTRIBUTION.ja.md) | [한국어](ATTRIBUTION.ko.md) | [Français](ATTRIBUTION.fr.md) | [Deutsch](ATTRIBUTION.de.md) | [Español](ATTRIBUTION.es.md) | [Italiano](ATTRIBUTION.it.md) | [Русский](ATTRIBUTION.ru.md) | [العربية](ATTRIBUTION.ar.md)

# Attribution et citation

Cette page traduit le [guide en anglais](../../ATTRIBUTION.md).
Les conditions de [LICENSE](../../LICENSE) font foi.

NeverD est développé par **NeverD contributors**. Son dépôt de code source est
[NeverSight/NeverD](https://github.com/NeverSight/NeverD).

## Obligations de licence lors de la réutilisation du code

Les éléments originaux de NeverD sont sous
[GNU AGPL version 3 uniquement](../../LICENSE). Lorsque vous transmettez des
copies ou adaptations couvertes par cette licence, conservez les mentions
applicables de droit d’auteur, de licence et de garantie, y compris la notice
du projet dans [NOTICE](../../NOTICE). Conservez également les éventuelles
mentions des auteurs individuels. Le code source modifié couvert par la licence
doit comporter des avis bien visibles indiquant les modifications et leur date.

Ces obligations s’appliquent aux éléments couverts réutilisés manuellement,
copiés ou adaptés avec un assistant d’IA ou un grand modèle de langage (LLM),
ou transformés au moyen de LLVM IR, d’une compilation, d’une décompilation
ou d’une conversion vers un autre langage de programmation. Modifier les noms,
la mise en forme, le langage ou les outils ne supprime pas en soi ces
obligations. Indiquez NeverD comme source des éléments de NeverD réutilisés ;
mentionner uniquement un modèle d’IA ou LLVM ne permet pas d’identifier cette
source.

Joignez la licence complète et les mentions applicables aux distributions du
code source et au code source correspondant des binaires distribués. Pour les
binaires et les services en réseau, respectez également les dispositions
applicables des sections 6 et 13 de l’AGPL. Une citation, un lien ou un
remerciement ne remplace **pas**, à lui seul, les exigences de l’AGPL relatives
à la licence, aux avis de modification ou à la mise à disposition du code source.

Ce guide explique la licence existante ; il n’ajoute aucune restriction ni
condition supplémentaire au titre de la section 7. Les conditions qui font foi
figurent dans [LICENSE](../../LICENSE), notamment dans les sections 0, 2, 4–6
et 13, également disponibles auprès de la
[Free Software Foundation](https://www.gnu.org/licenses/agpl-3.0.html).

## Rendre la source traçable

Pour chaque partie réutilisée, nous recommandons de consigner le fichier ou
symbole d’origine, le commit ou la version exacte, ainsi qu’une brève description
de vos modifications, à côté du code ou dans les notices de votre projet.
Utilisez un lien permanent GitHub avec le hachage complet du commit pour que
la citation continue d’identifier la même source. Ces précisions supplémentaires
sur la provenance constituent une recommandation de citation, et non une
condition de licence supplémentaire.

Par exemple, remplacez les champs entre crochets par les renseignements réels
sur la source :

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

Dans les processus assistés par l’IA, conservez ces informations de provenance
avec le contexte source sélectionné et reportez-les dans tout code couvert
que vous publiez. Vérifiez le code obtenu et ses mentions avant de le partager.
Pour les jeux de données contenant du code source NeverD couvert par la licence,
préservez les mentions et informations de licence applicables lorsque vous
transmettez ce code source.

## Recherche, références et résultats produits

Veuillez citer NeverD dans les articles, la documentation, les benchmarks et
les projets qui l’utilisent ou s’appuient sur son implémentation.
[CITATION.cff](../../CITATION.cff) fournit des métadonnées de citation du logiciel
lisibles par machine. La citation en texte brut ci-dessous peut être utilisée
avec la version ou le commit que vous avez réellement utilisé :

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

Le simple fait d’étudier une idée ou un algorithme ne soumet pas automatiquement
une implémentation indépendante à la licence de NeverD. De même, exécuter
NeverD sur le programme d’un tiers ne place pas automatiquement les résultats
produits sous l’AGPL : selon la section 2, un résultat n’est couvert que si
son contenu constitue une œuvre couverte par la licence. La même distinction
s’applique aux résultats de l’IA ; l’entraînement sur NeverD ou sa lecture
ne fait pas automatiquement de chaque résultat du modèle une œuvre couverte.
Pour ces usages sans éléments couverts, la citation est demandée au titre des
bonnes pratiques scientifiques et d’ingénierie, et non imposée comme nouvelle
condition de licence.

## Éléments tiers et copies antérieures

Les composants tels que LLVM, Capstone et Unicorn conservent leurs propres
licences. Consultez les mentions dans leur code source,
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) et toute licence propre
à un répertoire, y compris la
[licence du corpus de test](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE).
Conservez les attributions d’origine aux tiers et respectez leurs licences
lorsque vous réutilisez ces éléments. Ce guide ne change pas la licence des
éléments tiers et ne révoque pas les autorisations déjà accordées pour les
copies antérieures.
