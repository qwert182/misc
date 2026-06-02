#!/usr/bin/env python3
import sys, os, re, subprocess

class Node:
    @staticmethod
    def representer(dumper, node):
        d = node.__dict__.copy()
        if d["parent"] is not None:
            d["parent"] = d["parent"].name
        return dumper.represent_dict(d)

    def __init__(self, name, parent):
        self.name = name
        self.parent = parent

    def add_new_node(self, attr, name):
        if not hasattr(self, attr):
            setattr(self, attr, {})
        container = getattr(self, attr)
        if name in container:
            raise Exception(f"Got duplicate '{name}' in '{self}'")
        node = Node(name, self)
        container[name] = node
        return node

    def find(self, attr, name):
        result = []
        if hasattr(self, attr):
            container = getattr(self, attr)
            if name in container:
                result.append(container[name])
        for k, v in vars(self).items():
            if isinstance(v, dict):
                for sub_attr, sub_node in v.items():
                    result.extend(sub_node.find(attr, name))
        return result

class Parser:
    def __init__(self):
        self.root = Node("<root>", None)
        self.node = self.root
        self.prog = None
        self.parsers = {
            "gcc": self.parse_gcc,
            "as": self.parse_as,
            "collect2": self.parse_collect2,
            "ld": self.parse_ld,
        }
        self.in_comment = False
        self.last_opt_node = None
        self.in_known = False
        self.nodes_to_set_values = None
        self.ld_in_specific = False

    def parse(self, l):
        if self.prog is not None:
            if self.parsers[self.prog](l):
                return True
        m = re.match(r'Usage:\s*(\S*)', l, re.IGNORECASE)
        if m:
            self.prog = m.group(1).split("/")[-1]
            self.node = self.root.add_new_node("progs", self.prog)
            return True
        if re.match(r'Options:', l, re.IGNORECASE):
            return True
        if not l:
            if self.node and self.node.parent and self.node.parent.parent:
                self.node = self.node.parent
            assert self.node.parent == self.root
            return True
        raise Exception(f"cannot parse '{l}'")

    def parse_gcc(self, l):
        if self.in_comment:
            if re.match(r' \S+.*', l):
                return True
            assert not l
            self.in_comment = False
            return True
        if self.in_known:
            if l:
                for node in self.nodes_to_set_values:
                    if not hasattr(node, "values"):
                        node.values = l.split()
                    else:
                        node.values.extend(l.split())
            else:
                self.nodes_to_set_values = None
                self.in_known = False
            return True
        m = re.match(r'  (?:Known|Valid)\s+.*\s+(-\S+).*:', l, re.IGNORECASE)
        if m:
            self.in_known = True
            self.nodes_to_set_values = []
            for name in m.group(1).split("/"):
                existing_opts = self.root.find("opts", name)
                assert existing_opts
                assert len(existing_opts) == 1
                self.nodes_to_set_values.extend(existing_opts)
            return True

        m = re.match(r'  (\S+.*)', l)
        if m:
            o = m.group(1)
            m = re.match(r'(\S+.*?)\s+(.*)', o)
            if m:
                assert m.group(2)
                self.last_opt_node = self.node.add_new_node("opts", m.group(1))
                self.last_opt_node.description = m.group(2)
            else:
                self.last_opt_node = self.node.add_new_node("opts", o)
            return True
        m = re.match(r'  \s+(.*)', l)
        if m:
            if hasattr(self.last_opt_node, "description"):
                self.last_opt_node.description += " " + m.group(1)
            else:
                self.last_opt_node.description = m.group(1)
            return True
        self.last_opt_node = None

        if re.match(r'Options starting with .* are automatically', l, re.IGNORECASE):
            self.in_comment = True
            return True
        m = re.match(r'The following options are specific to just the language (\S*):', l, re.IGNORECASE)
        if m:
            self.node = self.node.add_new_node("lang", m.group(1))
            return True
        if re.match("The following options are language-related:", l, re.IGNORECASE):
            self.node = self.node.add_new_node("lang", "related")
            return True
        if re.match(r' None found.\s+.*', l, re.IGNORECASE):
            assert self.node.parent.name == "gcc"
            return True
        m = re.match(r'The (\S*) option recognizes the following as parameters:', l, re.IGNORECASE)
        if m:
            self.node = self.node.add_new_node("opts", m.group(1))
            return True
        m = re.match(r'The following options\s+(.*):', l, re.IGNORECASE)
        if m:
            self.node = self.node.add_new_node(
                "scope", {
                    "control compiler warning messages": "warnings",
                    "control optimizations": "optimizations",
                    "are target specific": "target",
                    "are language-independent": "language-independent",
                }[m.group(1)]
            )
            return True
        return False

    def parse_as(self, l):
        m = re.match(r'\s\s(\S+.*)', l)
        if m:
            o = m.group(1)
            m = re.match(r'(\S+.*?)\s+(.*)', o)
            if m and m.group(2):
                self.last_opt_node = self.node.add_new_node("opts", m.group(1))
                self.last_opt_node.description = m.group(2)
            else:
                self.last_opt_node = self.node.add_new_node("opts", o)
            return True
        m = re.match(r'\s\s\s+(.*)', l)
        if m:
            if hasattr(self.last_opt_node, "description"):
                self.last_opt_node.description += " " + m.group(1)
            else:
                self.last_opt_node.description = m.group(1)
            return True
        self.last_opt_node = None
        if re.match(r'Report bugs .*', l, re.IGNORECASE):
            return True
        return False

    def parse_collect2(self, l):
        if self.parse_as(l):
            return True
        if re.match(r'\s(\S+.*)', l):
            return True
        if re.match(r'Overview:|Report bugs:', l, re.IGNORECASE):
            return True
        return False

    def parse_ld(self, l):
        m = re.match(r'\s\s(-z \S+)\s*(.*)', l, re.IGNORECASE)
        if m:
            self.last_opt_node = self.node.add_new_node("opts", m.group(1))
            self.last_opt_node.description = m.group(2)
            return True
        if self.parse_as(l):
            return True
        if self.ld_in_specific:
            if self.node and self.node.parent and self.node.parent.parent:
                self.node = self.node.parent
            assert self.node.parent == self.root
            if re.match("ELF emulations:", l, re.IGNORECASE):
                self.node = self.node.add_new_node("emulation", "elf")
                return True
            m = re.match("(\S+):", l, re.IGNORECASE)
            if m:
                assert re.match("(elf|\S*pe)", m.group(1))
                self.node = self.node.add_new_node("emulation", m.group(1))
                return True
            assert not l
            self.ld_in_specific = False
            return True
        m = re.match(r'\S+ld:\s+supported (\S+):\s*(.*)', l, re.IGNORECASE)
        if m:
            node = self.node.add_new_node("supported", m.group(1))
            node.values = m.group(2).split()
            return True
        if re.match(r'\S+ld:\s+emulation specific options:$', l, re.IGNORECASE):
            self.ld_in_specific = True
            return True
        if re.match(r'(Report bugs |For bug |\S*/README.Bugs)', l, re.IGNORECASE):
            return True
        return False

def main():
    parser = Parser()
    try:
        with subprocess.Popen(["gcc", "--help", "-v"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env={**os.environ, "LANG": "C"},
                              universal_newlines=True) as process:
            stdout, stderr = process.communicate()
            if process.returncode != 0:
                raise Exception("process.returncode = {process.returncode}")

            node = None
            for l in stdout.split("\n"):
                parser.parse(l)
    finally:
        import yaml
        yaml.add_representer(Node, Node.representer, Dumper=yaml.SafeDumper)
        with open("/tmp/test.yml", "w") as y:
            print(yaml.safe_dump(parser.root), end="", file=y)

if __name__ == "__main__":
    main()
