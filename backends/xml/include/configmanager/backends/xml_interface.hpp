#ifndef CONFIGMANAGER_BACKENDS_XML_INTERFACE_HPP_
#define CONFIGMANAGER_BACKENDS_XML_INTERFACE_HPP_

#include "configmanager/config_interface.hpp"

namespace configmanager {

// XML backend (HighLevelDesign.md §6.1): restricted, elements-only mapping
// over pugixml, preserving member order end-to-end (ADR-022). The document
// root is <config version="N">; the carrier is a root *attribute*, outside
// the model's key space, so save()'s reserved-carrier check is vacuous
// (ADR-020) and a model member named "version" is plain data. The carrier
// value is decimal digits only (leading zeros permitted, no sign); anything
// else — or a value above 2^32-1, or a missing attribute — fails load() with
// InvalidVersion.
//
// Scalars are element text with a reserved `type` attribute (bool, int,
// double, null; absent means string — a bare empty element is the empty
// string). Literals are strict and untrimmed: bool is exactly true/false;
// int/double must consume their whole text, with int64 overflow and
// non-finite doubles failing load() with ParseError. An untyped element with
// child elements is an Object; an empty Object carries type="object" to stay
// distinguishable from the empty string; an Array carries type="array" and
// holds <item> children.
//
// load() fails with ParseError on: mixed content, text inside containers,
// child elements inside scalars, non-<item> array children, repeated sibling
// names under an object, unknown `type` values, any attribute other than the
// root's `version` and `type` (namespaces are unsupported), a root not named
// <config>, and non-path-addressable member names (ADR-021). save() fails
// with SerializationError on: model keys that are not valid element names
// (conservative ASCII subset [A-Za-z_][A-Za-z0-9_-]*), strings containing
// control characters other than tab and newline (XML 1.0 forbids most, and
// a carriage return cannot survive conformant line-ending normalization),
// and non-finite doubles.
class XmlInterface : public IConfigInterface {
 public:
  Result<VersionedConfig> load(std::istream& in) override;
  Result<void> save(const VersionedConfig& config,
                    std::ostream& out) override;
};

}  // namespace configmanager

#endif  // CONFIGMANAGER_BACKENDS_XML_INTERFACE_HPP_
