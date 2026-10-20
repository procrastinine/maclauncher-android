public final class ExitProbe {
    public static void main(String[] arguments) {
        Runtime.getRuntime().addShutdownHook(new Thread(() -> System.out.println("JAVA_HOOK")));
        System.exit(37);
    }
}
