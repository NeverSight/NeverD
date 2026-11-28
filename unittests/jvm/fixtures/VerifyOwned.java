import java.lang.classfile.ClassFile;
import java.nio.file.Files;
import java.nio.file.Path;

public final class VerifyOwned {
    public static void main(String[] args) throws Exception {
        ClassFile reader = ClassFile.of();
        for (String name : args) {
            var model = reader.parse(Files.readAllBytes(Path.of(name)));
            if (model.majorVersion() < 50) {
                System.out.println(model.majorVersion() + "." + model.minorVersion() + " parsed " + name);
                continue;
            }
            var errors = reader.verify(model);
            if (!errors.isEmpty()) throw new AssertionError(name + " " + errors);
            System.out.println(model.majorVersion() + "." + model.minorVersion() + " " + name);
        }
    }
}
