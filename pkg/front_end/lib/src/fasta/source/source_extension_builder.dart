// Copyright (c) 2019, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:kernel/ast.dart';

import '../../base/common.dart';
import '../builder/builder.dart';
import '../builder/declaration_builders.dart';
import '../builder/library_builder.dart';
import '../builder/metadata_builder.dart';
import '../builder/type_builder.dart';
import '../fasta_codes.dart'
    show
        messagePatchDeclarationMismatch,
        messagePatchDeclarationOrigin,
        noLength;
import '../kernel/body_builder_context.dart';
import '../problems.dart';
import '../scope.dart';
import 'name_scheme.dart';
import 'source_builder_mixins.dart';
import 'source_library_builder.dart';
import 'source_member_builder.dart';

class SourceExtensionBuilder extends ExtensionBuilderImpl
    with SourceDeclarationBuilderMixin {
  final Extension _extension;

  SourceExtensionBuilder? _origin;
  SourceExtensionBuilder? patchForTesting;

  MergedClassMemberScope? _mergedScope;

  @override
  final List<TypeVariableBuilder>? typeParameters;

  @override
  final TypeBuilder onType;

  final ExtensionName extensionName;

  SourceExtensionBuilder(
      List<MetadataBuilder>? metadata,
      int modifiers,
      this.extensionName,
      this.typeParameters,
      this.onType,
      Scope scope,
      SourceLibraryBuilder parent,
      int startOffset,
      int nameOffset,
      int endOffset,
      Extension? referenceFrom)
      : _extension = new Extension(
            name: extensionName.name,
            fileUri: parent.fileUri,
            typeParameters:
                TypeVariableBuilder.typeParametersFromBuilders(typeParameters),
            reference: referenceFrom?.reference)
          ..isExtensionTypeDeclaration = false
          ..isUnnamedExtension = extensionName.isUnnamedExtension
          ..fileOffset = nameOffset,
        super(metadata, modifiers, extensionName.name, parent, nameOffset,
            scope) {
    extensionName.attachExtension(_extension);
  }

  @override
  SourceLibraryBuilder get libraryBuilder =>
      super.libraryBuilder as SourceLibraryBuilder;

  bool get isUnnamedExtension => extensionName.isUnnamedExtension;

  @override
  SourceExtensionBuilder get origin => _origin ?? this;

  // TODO(johnniwinther): Add merged scope for extensions.
  MergedClassMemberScope get mergedScope => _mergedScope ??= isPatch
      ? origin.mergedScope
      : throw new UnimplementedError("SourceExtensionBuilder.mergedScope");

  @override
  Extension get extension => isPatch ? origin._extension : _extension;

  @override
  BodyBuilderContext get bodyBuilderContext =>
      new ExtensionBodyBuilderContext(this);

  @override
  Annotatable get annotatable => extension;

  /// Builds the [Extension] for this extension build and inserts the members
  /// into the [Library] of [libraryBuilder].
  ///
  /// [addMembersToLibrary] is `true` if the extension members should be added
  /// to the library. This is `false` if the extension is in conflict with
  /// another library member. In this case, the extension member should not be
  /// added to the library to avoid name clashes with other members in the
  /// library.
  Extension build(LibraryBuilder coreLibrary,
      {required bool addMembersToLibrary}) {
    _extension.onType = onType.build(libraryBuilder, TypeUse.extensionOnType);

    buildInternal(coreLibrary, addMembersToLibrary: addMembersToLibrary);

    return _extension;
  }

  @override
  void addMemberDescriptorInternal(
      SourceMemberBuilder memberBuilder,
      BuiltMemberKind memberKind,
      Reference memberReference,
      Reference? tearOffReference) {
    String name = memberBuilder.name;
    ExtensionMemberKind kind;
    switch (memberKind) {
      case BuiltMemberKind.Constructor:
      case BuiltMemberKind.RedirectingFactory:
      case BuiltMemberKind.Factory:
      case BuiltMemberKind.Field:
      case BuiltMemberKind.Method:
      case BuiltMemberKind.ExtensionTypeConstructor:
      case BuiltMemberKind.ExtensionTypeMethod:
      case BuiltMemberKind.ExtensionTypeGetter:
      case BuiltMemberKind.ExtensionTypeSetter:
      case BuiltMemberKind.ExtensionTypeOperator:
      case BuiltMemberKind.ExtensionTypeFactory:
      case BuiltMemberKind.ExtensionTypeRedirectingFactory:
        unhandled("${memberBuilder.runtimeType}:${memberKind}", "buildMembers",
            memberBuilder.charOffset, memberBuilder.fileUri);
      case BuiltMemberKind.ExtensionField:
      case BuiltMemberKind.LateIsSetField:
        kind = ExtensionMemberKind.Field;
        break;
      case BuiltMemberKind.ExtensionMethod:
        kind = ExtensionMemberKind.Method;
        break;
      case BuiltMemberKind.ExtensionGetter:
      case BuiltMemberKind.LateGetter:
        kind = ExtensionMemberKind.Getter;
        break;
      case BuiltMemberKind.ExtensionSetter:
      case BuiltMemberKind.LateSetter:
        kind = ExtensionMemberKind.Setter;
        break;
      case BuiltMemberKind.ExtensionOperator:
        kind = ExtensionMemberKind.Operator;
        break;
    }
    extension.members.add(new ExtensionMemberDescriptor(
        name: new Name(name, libraryBuilder.library),
        member: memberReference,
        tearOff: tearOffReference,
        isStatic: memberBuilder.isStatic,
        kind: kind));
  }

  @override
  void applyPatch(Builder patch) {
    if (patch is SourceExtensionBuilder) {
      patch._origin = this;
      if (retainDataForTesting) {
        patchForTesting = patch;
      }
      // TODO(johnniwinther): Check that type parameters and on-type match
      // with origin declaration.

      scope.forEachLocalMember((String name, Builder member) {
        Builder? memberPatch =
            patch.scope.lookupLocalMember(name, setter: false);
        if (memberPatch != null) {
          member.applyPatch(memberPatch);
        }
      });
      scope.forEachLocalSetter((String name, Builder member) {
        Builder? memberPatch =
            patch.scope.lookupLocalMember(name, setter: true);
        if (memberPatch != null) {
          member.applyPatch(memberPatch);
        }
      });
    } else {
      libraryBuilder.addProblem(messagePatchDeclarationMismatch,
          patch.charOffset, noLength, patch.fileUri, context: [
        messagePatchDeclarationOrigin.withLocation(
            fileUri, charOffset, noLength)
      ]);
    }
  }
}
